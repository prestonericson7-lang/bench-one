#!/usr/bin/env python3
"""board_experiment.py -- watch the PZ7020 boot on its serial console and run the FCLK0 experiment on the
board itself, so the cause of the 2026-10-05 stop is measured instead of inferred. Run on the PC:

    python board_experiment.py                 waits (up to --wait-hours) for the board's console (CH340, J2)
    python board_experiment.py --port COM7     a given port
    python board_experiment.py --port socket://localhost:5555    QEMU (qemu_serial_tcp.sh), to test this script
    python board_experiment.py --port socket://localhost:5556,socket://localhost:5555    port selection test

Every CH340 that appears is listened to (the STM32 boards carry CH340s too); the board's port is the one
that prints a Zynq boot or the board's Linux prompt. Nothing is sent until Linux has logged root in and
printed its report (a key during U-Boot's countdown would stop the boot). If the board was already up
when listening began (J2 plugged in after power), one Enter after 90 s of silence finds its prompt.
Every byte goes to captures/experiment-<time>/console-<port>.log; the findings to summary.txt there.
  0. the boot: up to the report, or the last lines before it went silent (a hang, located). Logged in but
     no report after 60 s of quiet: one Enter, and if the shell answers the steps go on;
  1. what this boot did: kernel command line, the guard's log (/boot/reports/plcheck.txt), services,
     fclk0 in the clock tree, the SLCR registers;
  2. if the guard's own PL read never returned, or it found a problem other than FCLK0: stop, read
     nothing more. If it blocked on FCLK0: clear the gate bit and read the PL (does it answer once the
     clock runs?). If it passed: read the PL's ID, its clock register, a write/read-back;
  3. FCLK0 measured: pl_regs' 64-bit counter runs on FCLK0; ticks over 1 s of CPU time;
  4. the gate test, no PL access while gated: counter, gate bit set for 2 s, cleared, counter.
     If the bit stops FCLK0, the counter comes out 2 s short of the wall clock;
  5. the stall test (skip with --no-stall; needs 2 CPUs): gate set, the PL read from CPU 1, the gate
     cleared from CPU 0 3 s later. Synced first: if the whole board hangs instead, it needs a power cycle,
     and this script listens for the reboot and carries on;
  6. restore: gate cleared, PL services restarted; a verdict computed from the numbers; results appended
     to /boot/reports/experiment.txt on the card.
"""
import argparse, base64, hashlib, os, re, sys, threading, time
import serial
import serial.tools.list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--port")
ap.add_argument("--wait-hours", type=float, default=12.0)
ap.add_argument("--no-stall", action="store_true")
ap.add_argument("--ignore-guard", action="store_true",
                help="QEMU only: run the PL steps even when the guard found problems other than FCLK0 "
                     "(QEMU has no PL; its reads return 0). Never on the board.")
ap.add_argument("--test-reboot", action="store_true",
                help="QEMU only: replace the stall test with a forced reboot, to exercise the reboot capture")
ap.add_argument("--verdict-only", metavar="FILE",
                help="print the verdicts for a file of PLX lines and exit (tests the verdict logic; no serial)")
ap.add_argument("--guard-only", metavar="FILE",
                help="print what step 1 makes of a plcheck.txt tail and exit (tests that parse; no serial)")
A = ap.parse_args()


def verdicts(allsum):
    """the verdict lines for the PLX output in allsum, computed only from its numbers"""
    out = []
    mhz = [float(x) for x in re.findall(r"PLX measure \d+ ticks in [0-9.]+ s = ([0-9.]+) MHz", allsum)]
    gt = re.findall(r"gate held ([0-9.]+) s .*? counter stopped for (-?[0-9.]+) s; counter afterwards ([0-9.]+) MHz", allsum)
    st = re.findall(r"PLX stall after 3 s with the gate set: \[(.*?)\]", allsum)
    sr = re.findall(r"PLX stall after the gate was cleared: \[(.*?)\](?: issued \[([0-9.]*)\])?", allsum)
    g1 = re.search(r"PLX gate 1 -> FPGA0_THR_CNT 0x[0-9a-f]+ at ([0-9.]+)", allsum)          # only the stall test sets it
    g0 = re.search(r"PLX gate 0 -> FPGA0_THR_CNT 0x[0-9a-f]+ at ([0-9.]+)", allsum[g1.end():]) if g1 else None
    sta = re.findall(r"FPGA0_THR_STA while gated (0x[0-9a-f]+), after (0x[0-9a-f]+)", allsum)
    if mhz and max(mhz) < 1:
        out.append("VERDICT FCLK0: the fabric's counter does not move (0 ticks) -- no PL answering at 0x40000000, "
                   "so nothing below says anything about FCLK0")
    elif mhz:
        out.append(f"VERDICT FCLK0 on the silicon: {', '.join('%.3f' % x for x in mhz)} MHz measured with the fabric's own counter")
    for held, stopped, after in gt:
        held, stopped, after = float(held), float(stopped), float(after)
        if after < 1:
            out.append("VERDICT gate test: no conclusion (the counter does not run)")
        elif abs(stopped - held) < 0.1:
            out.append(f"VERDICT gate test: the counter stopped for {stopped:.3f} s while the gate bit was held {held:.3f} s -- "
                       "the bit Linux sets at 'Disabling unused clocks' STOPS FCLK0")
        elif abs(stopped) < 0.05:
            out.append(f"VERDICT gate test: the counter kept running while the gate bit was held {held:.3f} s -- the bit does NOT "
                       "stop FCLK0, and the gating explanation of 2026-10-05 is WRONG")
        else:
            out.append(f"VERDICT gate test: counter short by {stopped:.3f} s for a {held:.3f} s gate -- partial, not understood")
    for g, a in sta:
        out.append(f"VERDICT FPGA0_THR_STA (the PS's own status) read {g} while gated and {a} after"
                   + (" -- it changed with the gate" if g != a else " -- no change"))
    if st and sr:
        done_while_set = " = 0x" in st[0]
        secs = re.search(r"in ([0-9.]+) s", sr[0][0])
        issued = float(sr[0][1]) if sr[0][1] else None
        cleared = float(g0.group(1)) if g0 else None
        if done_while_set:
            out.append("VERDICT stall test: the PL read returned while the gate was set (it took %s s) -- the gate "
                       "does not hold reads" % (secs.group(1) if secs else "?"))
        elif not secs:
            out.append("VERDICT stall test: the PL read did NOT return while the gate was set; it never returned")
        elif issued is not None and cleared is not None and issued >= cleared:
            out.append("VERDICT stall test: no conclusion -- the read was issued %.3f s AFTER the gate was cleared "
                       "(its python started late) and took %s s" % (issued - cleared, secs.group(1)))
        else:
            out.append("VERDICT stall test: the PL read did NOT return while the gate was set"
                       + (" (issued %.3f s before the gate was cleared)" % (cleared - issued) if issued is not None and cleared is not None else "")
                       + "; after the gate was cleared it returned, having taken %s s" % secs.group(1))
    elif "PLX stall froze the board" in allsum:
        out.append("VERDICT stall test: a PL read with the gate set froze the WHOLE board -- the console stopped "
                   "answering (the gate was to be cleared from the other CPU) and only a power cycle recovered it. Gate "
                   "set, then a PL read: the condition card #1's first boot was in when it stopped on 2026-10-05")
    for addr, name in re.findall(r"PLX guard read stuck: (0x[0-9a-f]+) \((.*?)\) --", allsum)[:1]:
        out.append(f"VERDICT boot: the guard's read of {addr} ({name}) never returned although FCLK0's gate bit was clear, "
                   "the level shifters on and the fabric out of reset -- something other than the FCLK0 gate stops that "
                   "read; the PL was not read again")
    return out


def guard_state(tail):
    """from the tail of /boot/reports/plcheck.txt: this boot's problems other than FCLK0, and the PL read the guard
    is stuck in, if any -- it writes and syncs 'reading 0x...' before each read and the value after it, so a
    boot whose last guard line is a 'reading' line has a read that never came back. On the board the lines come
    prefixed 'G|' and only those count: other console output (kernel stall warnings) can land among them"""
    if "G|" in tail:
        tail = "\n".join(l.split("G|", 1)[1] for l in tail.splitlines() if "G|" in l)
    this_boot = tail.split("== boot")[-1]
    other = [l for l in this_boot.splitlines() if "problem:" in l and "FCLK0 gated" not in l]
    lines = [l for l in this_boot.splitlines() if l.strip()]
    m = re.search(r"reading (0x[0-9a-f]{8}) \((.*)\) through M_AXI_GP0", lines[-1]) if lines else None
    return other, (m.groups() if m else None)


if A.verdict_only:
    print("\n".join(verdicts(open(A.verdict_only, encoding="utf-8").read())) or "(no PLX lines)")
    sys.exit(0)
if A.guard_only:
    o, s = guard_state(open(A.guard_only, encoding="utf-8").read())
    print(f"other={len(o)} stuck={s[0] + ' ' + s[1] if s else None}")
    sys.exit(0)

OUT = os.path.join(HERE, "captures", time.strftime("experiment-%Y%m%d-%H%M%S"))
try:
    os.makedirs(OUT)
except FileExistsError:                       # two runs started in the same second (test_driver_e2e.sh runs pairs):
    OUT += "-%d" % os.getpid(); os.makedirs(OUT)   # never share a summary.txt, whose lines become the verdicts
SUM = os.path.join(OUT, "summary.txt")
lock = threading.Lock(); stop = [False]
# established connections to the board's nbd-server (TCP 10809 = 0x2A39), counted on the server's side: the
# Orange Pi's swap lives in this board's RAM (export "zynqram", priority 100 on the Pi, so it fills first)
PI_ATTACHED_CMD = "awk '$2 ~ /:2A39$/ && $4 == \"01\"' /proc/net/tcp | wc -l"
# what only this board prints: the SPL's and U-Boot's banners, the kernel, the board's model, its Linux
# prompt and report. The SPL's banner ("U-Boot SPL 2024.01 ...") is its first line: without it a boot that
# stopped in the first stage would never be recognised as the board, and nothing would be reported.
MARK = r"U-Boot (SPL )?20\d\d|Booting Linux|Puzhi PZ7020|zynq login|root@zynq|ZYNQ-REPORT"


def note(m):
    line = time.strftime("%H:%M:%S  ") + m
    print(line, flush=True)
    with open(SUM, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def tell_owner(title, text, timeout_ms=0):
    """a beep and a message box on this PC, without blocking the run: the board's HDMI cannot show a frozen
    CPU (scanout runs on the GPU's own clock), so this is how the owner learns to power-cycle it.
    timeout_ms > 0 closes the box by itself (used by the self-test)."""
    note(f"OWNER: {title}: {text}")
    if os.name != "nt":
        return

    def show():
        try:
            import ctypes, winsound
            for _ in range(3):
                winsound.Beep(1200, 250); time.sleep(0.1)
            u32 = ctypes.windll.user32
            if timeout_ms:                            # MessageBoxTimeoutW: closes itself after timeout_ms
                u32.MessageBoxTimeoutW(None, text, title, 0x40 | 0x40000, 0, timeout_ms)
            else:
                u32.MessageBoxW(None, text, title, 0x40 | 0x40000)      # MB_ICONINFORMATION | MB_TOPMOST
        except Exception as e:
            note(f"(could not show the message box: {e})")
    threading.Thread(target=show, daemon=False).start()


def open_port(name, tries=240):
    if "://" in name:                         # QEMU / a test server: retry until it listens
        for _ in range(tries):
            try:
                return serial.serial_for_url(name, baudrate=115200, timeout=0.2)
            except Exception:
                time.sleep(1)
        raise OSError(f"cannot connect to {name}")
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = name, 115200, 0.2
    s.dtr = False; s.rts = False              # not asserted, as watch_boot.ps1 does
    s.open()
    return s


class Port:
    """one serial port being listened to; every byte also goes to console-<port>.log"""
    def __init__(self, name):
        self.name, self.buf, self.alive = name, bytearray(), True
        self.s = open_port(name)
        self.opened = self.last = time.time()
        self.raw = open(os.path.join(OUT, "console-%s.log" % re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_")), "ab")
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        while not stop[0] and self.alive:
            try:
                c = self.s.read(4096)
            except Exception as e:            # the port went away: J2 replugged, or (QEMU) the emulator restarted
                if not self.alive or stop[0]:
                    return
                note(f"{self.name}: serial read failed ({e}) -- reopening it as soon as it is back")
                try:
                    self.s.close()
                except Exception:
                    pass
                end = time.time() + 7200
                while time.time() < end and self.alive and not stop[0]:
                    time.sleep(1)
                    try:
                        self.s = open_port(self.name, tries=1)
                        note(f"{self.name}: reopened")
                        break
                    except Exception:
                        continue
                else:
                    self.alive = False
                    return
                continue
            if c:
                self.raw.write(c); self.raw.flush()
                with lock:
                    self.buf.extend(c); self.last = time.time()

    def text(self):
        with lock:
            t = self.buf.decode("utf-8", "replace")
        return re.sub(r"\x1b\[[?0-9;]*[A-Za-z]", "", t).replace("\r", "")

    def close(self):
        self.alive = False
        try:
            self.s.close()
        except Exception:
            pass


def candidates():
    if A.port:                                # a port, or a comma-separated list (the selection test)
        return [x for x in A.port.split(",") if x]
    return [p.device for p in serial.tools.list_ports.comports() if p.vid == 0x1A86]


# ---- which port is the board
ports, probed, failed = {}, set(), {}
note("listening on " + A.port if A.port else
     f"waiting up to {A.wait_hours} h for the board's console: plug J2 (lower USB-C) into the PC, then power J8")
deadline = time.time() + A.wait_hours * 3600
P = None
initial, warned = set(candidates()), set()    # ports present before the owner plugged anything in
while P is None:
    for name in candidates():
        # a port that would not open is tried again every 5 s: right after plug-in Windows may still be
        # binding the CH340 driver, or another program may hold it for a moment (the first version gave
        # up on it for good)
        if name not in ports and time.time() - failed.get(name, 0) > 5:
            try:
                ports[name] = Port(name); note(f"listening on {name}"); failed.pop(name, None)
            except Exception as e:
                if name not in failed:
                    note(f"{name}: cannot open yet ({e}); trying again every 5 s")
                failed[name] = time.time()
    for p in [x for x in ports.values() if x]:
        if re.search(MARK, p.text()):
            P = p; break
        # quiet ever since it opened: the board may already be up (J2 plugged in after power). One Enter shows
        # a prompt. Never sooner: a running boot prints U-Boot's banner first, so no key reaches its countdown.
        if p.name not in probed and time.time() - p.opened > 90 and time.time() - p.last > 30:
            probed.add(p.name)
            try:
                p.s.write(b"\r")
                note(f"{p.name}: nothing from a Zynq for 90 s -- sent one Enter in case Linux is already up")
            except Exception as e:
                note(f"{p.name}: write failed ({e})")
        # a port plugged in after the start that has sent not one byte in 10 min: a board whose first stage
        # prints nothing (2026-09-24) would otherwise leave the owner waiting with no sign anything is wrong
        with lock:
            empty = not p.buf
        if empty and p.name not in initial and p.name not in warned and time.time() - p.opened > 600:
            warned.add(p.name)
            tell_owner("Nothing heard from the board yet",
                       f"{p.name} (plugged in 10 minutes ago) has sent nothing at all. If it is FPGA #1's J2 and J8 has "
                       "power, the board is not even starting its first stage: check the charger on J8 and that card #1 "
                       "is pushed fully in. If J8 is not powered yet, power it now. The PC keeps listening.")
    if P is None:
        if time.time() > deadline:
            note("no board console appeared"); sys.exit(4)
        time.sleep(1)
for p in ports.values():
    if p and p is not P:
        p.close(); note(f"{p.name} is not the board -- closed")
note(f"the board's console is {P.name}; capture in {OUT}")


def send(b):
    P.s.write(b); P.s.flush()


def text():
    return P.text()


def wait_for(pattern, limit, start=0):
    """index just past the first match of pattern in text()[start:], or None after limit seconds"""
    end = time.time() + limit
    while time.time() < end and not stop[0]:
        m = re.search(pattern, text()[start:])
        if m:
            return start + m.end()
        time.sleep(0.2)
    return None


def watch_boot(start, quiet_before_kernel):
    """listen (sending nothing) from text index start until the report, a panic, or silence; True if up"""
    seen, t0, poked, mark, pokedat = {}, time.time(), False, 0, 0.0
    while True:
        t = text()[start:]
        for key, pat in (("SPL", r"U-Boot SPL 20\d\d"), ("U-Boot", r"U-Boot 20\d\d"), ("pl.bit", r"Loading PL bitstream"),
                         ("PL NOT loaded", r"fpga loadb of pl.bit FAILED|No pl.bit on the boot partition"),
                         ("kernel", r"Booting Linux"),
                         ("clk pass", r"clk: Disabling unused clocks"), ("systemd", r"systemd\[1\]"),
                         ("guard", r"zynq-plcheck|verdict:"), ("login", r"root@zynq|automatic login"),
                         ("report", r"ZYNQ-REPORT END"), ("panic", r"Kernel panic|Internal error|Unable to handle")):
            if key not in seen and re.search(pat, t):
                seen[key] = time.time() - t0
                note(f"  boot: {key} at +{seen[key]:.1f} s")
        if "report" in seen or "panic" in seen:
            break
        if "login" in seen and P.name in probed and time.time() - P.last > 20:
            note("  boot: the board was already up when the PC began listening -- its boot was not observed")
            return True
        # logged in 60 s ago and no report: zynq-report (8 s after login) waits for the guard, and the guard waits
        # for its PL read with no timeout, on purpose. If that read hung on one CPU the other may still run the
        # shell: ask it once, rather than calling the board stopped and losing what it can still say (in QEMU,
        # with the guard's read held, the console went quiet and the silence rule below called a live board
        # stopped). Timed from the login, not from silence: on silicon a CPU stuck in a bus read cannot pass
        # through RCU, so the kernel is expected to print stall warnings (CONFIG_RCU_CPU_STALL_TIMEOUT=21)
        if "login" in seen and not poked and time.time() - t0 - seen["login"] > 60:
            poked, mark, pokedat = True, len(text()), time.time()
            note("  boot: logged in 60 s ago and no report -- one Enter: does the shell still answer?")
            send(b"\r")
        # the prompt anywhere after the Enter: kernel messages can land on the same line ("\r" is dropped)
        if poked and re.search(r"root@zynq\d*:\S*#", text()[mark:]):
            note("  boot: the shell answers, but the report never printed: something it waits for is stuck "
                 "(the guard's log, read next, says what)")
            return True
        if poked and time.time() - pokedat > 600:
            note("  boot: no prompt in the 10 min since the Enter, and no report. Its last console lines:")
            for ln in t.splitlines()[-40:]:
                note("  | " + ln)
            return False
        quiet = time.time() - P.last
        if quiet > (150 if "kernel" in seen else quiet_before_kernel):
            note(f"  boot: SILENT for {quiet:.0f} s -- the board stopped. Its last console lines:")
            for ln in t.splitlines()[-40:]:
                note("  | " + ln)
            return False
        time.sleep(0.5)
    if "panic" in seen:
        time.sleep(3)
        for ln in text()[start:].splitlines()[-40:]:
            note("  | " + ln)
        note("  boot: kernel panic")
        return False
    for ln in t.split("==== ZYNQ-REPORT BEGIN ====")[-1].split("==== ZYNQ-REPORT END ====")[0].splitlines():
        if ln.strip():
            note("  report | " + re.sub(r"^\[ *[0-9.]+\] python3\[\d+\]: ", "", ln))
    return True


def await_shell():
    time.sleep(2); send(b"\r")
    if wait_for(r"root@zynq\d*:[^\n]*# ?$", 60, max(0, len(text()) - 2000)) is None:
        send(b"\r"); time.sleep(2)


n = [0]


def run(cmd, limit=120, show=True):
    """run one shell line; returns its output (between the command echo and a unique sentinel), or None"""
    n[0] += 1
    tag = "__S%dE__" % n[0]
    start = len(text())
    send((cmd + "; echo '__S%d''E__' $?\r" % n[0]).encode())
    end = wait_for(re.escape(tag) + r" (\d+)", limit, start)
    if end is None:
        note(f"  NO ANSWER within {limit} s to: {cmd}")
        return None
    t = text()[start:end]
    body = t.split("\n", 1)[1] if "\n" in t else ""
    body = body[:body.rfind(tag)].strip("\n")
    if show:
        for ln in body.splitlines():
            if ln.strip():
                note("  | " + ln)
    return body


def send_plx():
    src = open(os.path.join(HERE, "plx.py"), "rb").read().replace(b"\r\n", b"\n")
    b64 = base64.b64encode(src).decode()
    run("rm -f /tmp/plx.b64", show=False)
    for i in range(0, len(b64), 600):
        run("printf '%%s' '%s' >> /tmp/plx.b64" % b64[i:i + 600], show=False)
    got = run("base64 -d /tmp/plx.b64 > /tmp/plx.py && sha256sum /tmp/plx.py | cut -c1-64", show=False) or ""
    want = hashlib.sha256(src).hexdigest()
    if want not in got:
        note(f"plx.py did not arrive intact ({got.strip()} != {want}) -- stopping"); sys.exit(5)
    note(f"  plx.py on the board, sha256 {want[:16]} matches")


def guarded_read(addr, label):
    """read a PL register in a background process; report a hang instead of hanging this session"""
    out = run("(python3 /tmp/plx.py rdtimed %s > /tmp/rd.out 2>&1 &) ; sleep 5; cat /tmp/rd.out || true" % addr, 60)
    if out is None:
        note(f"  {label}: the board stopped answering after the read was issued")
        return None
    if "PLX rdtimed" not in out:
        note(f"  {label}: the read has NOT returned after 5 s (a normal read takes microseconds)")
    return out


# ---- 0. the boot: listen only
note("step 0: listening to the boot (sending nothing)")
if not watch_boot(0, 600):
    if not A.port or "socket://" not in A.port:
        tell_owner("FPGA #1 stopped during its boot",
                   "The PC recorded the boot up to the moment it stopped (hardware\\pz7020-starlite\\linux\\captures, "
                   "newest experiment folder). Nothing more runs this time. Leave the cards as they are.")
    stop[0] = True; sys.exit(3)
await_shell()
run("stty cols 4000 2>/dev/null; true", show=False)       # no readline wrapping of long command lines

# ---- 1. what this boot did
note("step 1: what this boot did")
run("cat /proc/cmdline; uname -r; cat /proc/uptime")
guard = run("tail -14 /boot/reports/plcheck.txt | sed 's/^/G|/'") or ""
other, stuck = guard_state(guard)
if stuck:
    note(f"  PLX guard read stuck: {stuck[0]} ({stuck[1]}) -- the guard's last line is that read, and no value followed")
run("systemctl is-active zynq-plcheck fpgagpud zaccel-server zynq-agent nbd-server | tr '\\n' ' '; echo")
# fclk0's own lines: clk_summary lists fclk3 and fclk2 first, so the first version's "head -8" never got to it
run("grep -E ' clock |fclk[01]' /sys/kernel/debug/clk/clk_summary")
# memory with the Pi's swap export on: the two-board emulation ran out at 57 MB available; this is the board's figure
run("free -m | head -2; grep -E 'MemAvailable|Shmem:|CmaFree' /proc/meminfo")
send_plx()
regs = run("python3 /tmp/plx.py regs") or ""
m = re.search(r"FPGA0_THR_CNT 0x([0-9a-f]{8})", regs)
gated = bool(m and int(m.group(1), 16) & 1)
loaded = "fpgagpu.pl_loaded=1" in (run("cat /proc/cmdline", show=False) or "")
note(f"  FCLK0 gate bit {'SET (clock stopped)' if gated else 'clear'}; PL loaded at boot: {loaded}")
if "== boot" not in guard:
    note("  no guard log for this boot (/boot/reports/plcheck.txt): the driver's own register reading decides alone")
# the same two checks as the guard, from this driver's own register reading: a missing or unreadable guard log
# must not let the PL be read with the level shifters off or the fabric held in reset
lv, rs = re.search(r"LVL_SHFTR_EN 0x([0-9a-f]{8})", regs), re.search(r"FPGA_RST_CTRL 0x([0-9a-f]{8})", regs)
if not loaded:
    pass                                      # no bitstream at this boot: nothing will be read below
elif not (lv and rs):
    other.append("problem (driver): the PS registers could not be read, so the bus state is unknown")
else:
    if int(lv.group(1), 16) & 0xF != 0xF and not any("level shifters" in l for l in other):
        other.append(f"problem (driver): PS/PL level shifters not all on: LVL_SHFTR_EN 0x{int(lv.group(1), 16):x}")
    if int(rs.group(1), 16) & 1 and not any("fabric reset" in l for l in other):
        other.append(f"problem (driver): fabric reset 0 asserted: FPGA_RST_CTRL 0x{int(rs.group(1), 16):x}")

# ---- 2.
refused = False
if stuck:
    # the read is still outstanding on one CPU; another PL read could stick the second one too
    note("the guard's PL read never returned although its checks passed -- the PL is NOT read again. The registers "
         "above are what the PS saw; the console log has the boot")
    loaded = gated = False; refused = True
elif other and not A.ignore_guard:
    note("a problem other than FCLK0 was found -- the PL is NOT read (it could hang the board for a reason this "
         "experiment does not test):")
    for l in other:
        note("  " + l.strip())
    loaded = gated = False; refused = True
elif other:
    note("--ignore-guard: going on despite: " + " / ".join(l.strip() for l in other))
if gated:
    note("step 2: the guard found FCLK0 gated. Clearing the gate bit, then reading the PL")
    run("python3 /tmp/plx.py gate 0")
    guarded_read("0x40000000", "PL ID read after clearing the gate")
elif loaded:
    note("step 2: FCLK0 running: the PL's ID, its clock register, a write/read-back")
    run("python3 /tmp/plx.py rd 0x40000000; python3 /tmp/plx.py rd 0x40000020")
    run("python3 -c \"import mmap,os,struct;fd=os.open('/dev/mem',os.O_RDWR|os.O_SYNC);m=mmap.mmap(fd,4096,offset=0x40000000);"
        "struct.pack_into('<I',m,0x1C,0x1234ABCD);print('PLX scratch wrote 0x1234abcd read 0x%08x'%struct.unpack_from('<I',m,0x1C)[0])\"")
elif refused:
    note("steps 2-5 skipped: what stopped the bus above must be understood first")
else:
    note("step 2: no PL loaded at this boot (no marker) -- the PL steps below read nothing real")

# ---- 3. and 4.
if loaded or gated:
    note("step 3: FCLK0 measured on the silicon (pl_regs counter over 1 s of CPU time)")
    for _ in range(3):
        run("python3 /tmp/plx.py measure 1")
    note("step 4: the gate test -- no PL access while the gate bit is set")
    run("systemctl stop fpgagpud zaccel-server zynq-agent; systemctl is-active fpgagpud zaccel-server zynq-agent | tr '\\n' ' '; echo")
    for _ in range(2):
        run("python3 /tmp/plx.py gatetest 2")
    # ---- 5.
    ncpu = (run("nproc", show=False) or "1").strip().splitlines()[-1]
    out = ""
    if A.test_reboot:
        note("step 5 replaced (--test-reboot): the board is rebooted to exercise the reboot capture")
        send(b"sync; reboot -f\r"); out = None
    elif not A.no_stall and ncpu.isdigit() and int(ncpu) < 2:
        note(f"step 5 skipped: {ncpu} CPU -- a stuck read would freeze the only core with nothing left to release it")
    elif not A.no_stall and (run(PI_ATTACHED_CMD, show=False) or "0").strip().splitlines()[-1:] != ["0"]:
        # the export file is fully allocated, so how much the Pi keeps there cannot be seen from here: that it is
        # attached is enough. A freeze would lose those pages under the Pi's processes
        note("step 5 skipped: the Orange Pi is attached to this board's RAM export (its swap, used first). If the "
             "stall test froze the board, the Pi would lose the pages it keeps there. The gate test above already "
             "answers whether the bit stops FCLK0; for the stall test, run this again with the Pi switched off")
    elif not A.no_stall:
        note("step 5: the stall test -- the PL read with the gate set, released from the other CPU after 3 s")
        run("sync; echo synced")
        # everything but the reader stays on CPU 0: CPU 1 is the one expected to stall
        # every moment on CLOCK_MONOTONIC (the gate writes, the read's issue and its end), so the verdict can
        # tell a read that waited for the gate from one whose python started only after the gate was cleared
        out = run("rm -f /tmp/stall.out /tmp/stall.issued; taskset -c 0 sh -c 'python3 /tmp/plx.py gate 1; "
                  "(taskset -c 1 python3 /tmp/plx.py rdtimed 0x40000004 /tmp/stall.issued > /tmp/stall.out 2>&1 &); sleep 3; "
                  "echo \"PLX stall after 3 s with the gate set: [$(cat /tmp/stall.out 2>/dev/null)] issued [$(cat /tmp/stall.issued 2>/dev/null)]\"; "
                  "python3 /tmp/plx.py gate 0; sleep 2; echo \"PLX stall after the gate was cleared: [$(cat /tmp/stall.out)] "
                  "issued [$(cat /tmp/stall.issued 2>/dev/null)]\"'", 90)
    if out is None:                           # the board stopped answering (or --test-reboot rebooted it)
        if not A.test_reboot:
            # what CPU 0 managed to print before it stopped (not the echo of the typed command, which has "$(")
            said = re.findall(r"PLX stall after 3 s with the gate set: \[(?!\$\()[^\]]*\] issued \[(?!\$\()[^\]]*\]", text()[-20000:])
            note("  PLX stall froze the board: no console answer for 90 s after CPU 1 read the PL with the gate set; "
                 + (f"CPU 0 still printed <{said[-1]}> and then stopped too, at or after clearing the gate" if said else
                    "CPU 0, which was to clear the gate after 3 s, printed nothing either"))
        note("  the board stopped answering: power-cycle it (it boots the same card). Listening for the reboot, "
             "up to 60 min, sending nothing")
        if not A.test_reboot:
            tell_owner("FPGA #1 froze in the last test (expected possibility)",
                       "Please power-cycle FPGA #1: unplug the upper USB-C (J8) for 5 seconds and plug it back in. "
                       "Leave J2 in the PC. The PC is still listening and will record the reboot.")
        if watch_boot(len(text()), float(os.environ.get("BOARD_EXP_REBOOT_WAIT", 3600))):   # env: tests only
            await_shell(); run("stty cols 4000 2>/dev/null; true", show=False)
            note("  the board is back up on the same card")
            send_plx()
        else:
            # the measurements of steps 3 and 4 are already in the summary: their verdicts still count
            vs = verdicts(open(SUM, encoding="utf-8").read())
            for v in vs:
                note(v)
            if not A.test_reboot:
                tell_owner("FPGA #1 did not come back after the power cycle",
                           "The measurements before the freeze are saved (hardware\\pz7020-starlite\\linux\\captures, "
                           "newest experiment folder):\n\n" + "\n\n".join(v.replace("VERDICT ", "") for v in vs))
            stop[0] = True; sys.exit(3)
    # ---- 6.
    note("step 6: restore")
    run("python3 /tmp/plx.py gate 0; python3 /tmp/plx.py regs; python3 /tmp/plx.py lockstate; python3 /tmp/plx.py rd 0x40000000")
    run("systemctl start fpgagpud zaccel-server zynq-agent; sleep 2; systemctl is-active fpgagpud zaccel-server zynq-agent | tr '\\n' ' '; echo")
    run("dmesg | grep -i -E 'rcu|stall|lockup|bus|abort' | tail -6")

# ---- the verdict, computed from the numbers above
vs = verdicts(open(SUM, encoding="utf-8").read())
for v in vs:
    note(v)
skips = [l.split("  ", 1)[1] for l in open(SUM, encoding="utf-8").read().splitlines()
         if re.search(r"^\S+  steps? [0-9-]+ skipped", l)]
if not A.port or "socket://" not in A.port:   # a real board: tell the owner it is finished
    tell_owner("FPGA #1 experiment finished", "\n\n".join([v.replace("VERDICT ", "") for v in vs] + skips) or
               "No PL measurements were possible; see the summary in hardware\\pz7020-starlite\\linux\\captures.")
lines = [l.split("  ", 1)[1] for l in open(SUM, encoding="utf-8").read().splitlines() if "PLX " in l or "boot:" in l or "VERDICT" in l]
# onto the card in 600-character base64 pieces, each answered before the next, as plx.py went over: one 8 KB
# burst typed at 115200 baud with no flow control is not something to trust to the board's input buffer
b64 = base64.b64encode(("== experiment " + time.strftime("%Y-%m-%d %H:%M (PC time)") + "\n" + "\n".join(lines + skips)
                        + "\n").encode()).decode()
run("rm -f /tmp/exp.b64", show=False)
for i in range(0, len(b64), 600):
    run("printf '%%s' '%s' >> /tmp/exp.b64" % b64[i:i + 600], show=False)
run("mkdir -p /boot/reports; base64 -d /tmp/exp.b64 >> /boot/reports/experiment.txt && sync; tail -n 3 /boot/reports/experiment.txt")
note(f"done; console logs and this summary in {OUT}")
stop[0] = True
