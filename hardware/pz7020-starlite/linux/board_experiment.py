#!/usr/bin/env python3
"""board_experiment.py -- watch the PZ7020 boot on its serial console and run the FCLK0 experiment on the
board itself, so the cause of the 2026-10-05 stop is measured instead of inferred. Run on the PC:

    python board_experiment.py                 waits (up to --wait-hours) for the board's CH340 (J2)
    python board_experiment.py --port COM7     a given port
    python board_experiment.py --port socket://localhost:5555    QEMU (qemu_serial_tcp.sh), to test this script

It sends NOTHING until Linux has logged root in and printed its report (a key during U-Boot's countdown
would stop the boot). Every byte goes to captures/experiment-<time>/console.log; the findings to
summary.txt there. Steps, each printed with the board's own numbers:
  0. the boot: up to the report, or the last lines before it went silent (a hang, located);
  1. what this boot did: kernel command line, the guard's log (/boot/reports/plcheck.txt), services,
     fclk0 in the clock tree, the SLCR registers;
  2. if the guard blocked (FCLK0 gated): clear the gate bit and read the PL -- does it answer once the
     clock runs?  If the guard passed: read the PL's ID, its clock register, a write/read-back;
  3. FCLK0 measured: pl_regs' 64-bit counter runs on FCLK0; ticks over 1 s of CPU time;
  4. the gate test, no PL access while gated: counter, gate bit set for 2 s, cleared, counter.
     If the bit stops FCLK0, the counter comes out 2 s short of the wall clock;
  5. the stall test (skip with --no-stall): gate set, the PL read from CPU 1, the gate cleared from CPU 0
     3 s later. The hypothesis says the read hangs until the clock returns. Synced first, because the
     whole board may hang instead -- then the console goes silent and the board needs a power cycle;
  6. restore: gate cleared, PL services restarted, results appended to /boot/reports/experiment.txt.
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
A = ap.parse_args()

OUT = os.path.join(HERE, "captures", time.strftime("experiment-%Y%m%d-%H%M%S"))
os.makedirs(OUT, exist_ok=True)
RAW = open(os.path.join(OUT, "console.log"), "ab")
SUM = os.path.join(OUT, "summary.txt")
buf = bytearray(); lock = threading.Lock(); last_rx = [time.time()]; stop = [False]


def note(m):
    line = time.strftime("%H:%M:%S  ") + m
    print(line, flush=True)
    with open(SUM, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def find_port():
    if A.port:
        return A.port
    note(f"waiting up to {A.wait_hours} h for the board's CH340 (USB VID 1A86) -- plug J2 into the PC")
    end = time.time() + A.wait_hours * 3600
    while time.time() < end:
        for p in serial.tools.list_ports.comports():
            if p.vid == 0x1A86:
                note(f"found {p.device} ({p.description})")
                return p.device
        time.sleep(1)
    note("no CH340 appeared"); sys.exit(4)


def open_port(name):
    if "://" in name:                         # QEMU: retry until it listens (it copies the image first)
        for _ in range(240):
            try:
                return serial.serial_for_url(name, baudrate=115200, timeout=0.2)
            except Exception:
                time.sleep(1)
        note(f"cannot connect to {name}"); sys.exit(4)
    else:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = name, 115200, 0.2
        s.dtr = False; s.rts = False          # not asserted, as watch_boot.ps1 does
        s.open()
    return s


def reader(s):
    while not stop[0]:
        try:
            c = s.read(4096)
        except Exception as e:                # port vanished: the board lost power or the cable moved
            note(f"serial read failed: {e}"); stop[0] = True; return
        if c:
            RAW.write(c); RAW.flush()
            with lock:
                buf.extend(c); last_rx[0] = time.time()


def text():
    with lock:
        t = buf.decode("utf-8", "replace")
    return re.sub(r"\x1b\[[?0-9;]*[A-Za-z]", "", t).replace("\r", "")


def wait_for(pattern, limit, start=0):
    """index just past the first match of pattern in text()[start:], or None after limit seconds"""
    end = time.time() + limit
    while time.time() < end and not stop[0]:
        m = re.search(pattern, text()[start:])
        if m:
            return start + m.end()
        time.sleep(0.2)
    return None


port = find_port()
note(f"opening {port} at 115200 8N1; capture in {OUT}")
s = open_port(port)
threading.Thread(target=reader, args=(s,), daemon=True).start()

# ---- 0. the boot: listen only
note("step 0: listening to the boot (sending nothing)")
first = wait_for(r"\S", A.wait_hours * 3600)
t_first = time.time()
seen = {}
while True:
    t = text()
    for key, pat in (("U-Boot", r"U-Boot 20\d\d"), ("pl.bit", r"Loading PL bitstream"), ("kernel", r"Booting Linux"),
                     ("clk pass", r"clk: Disabling unused clocks"), ("systemd", r"systemd\[1\]"),
                     ("guard", r"zynq-plcheck|verdict:"), ("login", r"root@zynq|automatic login"),
                     ("report", r"ZYNQ-REPORT END"), ("panic", r"Kernel panic|Internal error|Unable to handle")):
        if key not in seen and re.search(pat, t):
            seen[key] = time.time() - t_first
            note(f"  boot: {key} at +{seen[key]:.1f} s")
    if "report" in seen or "panic" in seen:
        break
    quiet = time.time() - last_rx[0]
    if quiet > (150 if "kernel" in seen else 600):
        note(f"  boot: SILENT for {quiet:.0f} s -- the board stopped. Its last console lines:")
        for ln in t.splitlines()[-40:]:
            note("  | " + ln)
        stop[0] = True; sys.exit(3)
    time.sleep(0.5)
if "panic" in seen:
    time.sleep(3)
    for ln in text().splitlines()[-40:]:
        note("  | " + ln)
    note("kernel panic -- stopping"); stop[0] = True; sys.exit(3)
for ln in text().split("==== ZYNQ-REPORT BEGIN ====")[-1].split("==== ZYNQ-REPORT END ====")[0].splitlines():
    if ln.strip():
        note("  report | " + re.sub(r"^\[ *[0-9.]+\] python3\[\d+\]: ", "", ln))

# ---- the shell
time.sleep(2); s.write(b"\r")
if wait_for(r"root@zynq\d*:[^\n]*# ?$", 60, len(text()) - 2000 if len(text()) > 2000 else 0) is None:
    s.write(b"\r"); time.sleep(2)
n = [0]


def run(cmd, limit=120, show=True):
    """run one shell line; returns its output (between the command echo and a unique sentinel), or None"""
    n[0] += 1
    tag = "__S%dE__" % n[0]
    start = len(text())
    s.write((cmd + "; echo '__S%d''E__' $?\r" % n[0]).encode())
    s.flush()
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


run("stty cols 4000 2>/dev/null; true", show=False)       # no readline wrapping of long command lines
# ---- 1. what this boot did
note("step 1: what this boot did")
run("cat /proc/cmdline; uname -r; cat /proc/uptime")
guard = run("tail -14 /boot/reports/plcheck.txt") or ""
this_boot = guard.split("== boot")[-1]
other = [l for l in this_boot.splitlines() if "problem:" in l and "FCLK0 gated" not in l]
run("systemctl is-active zynq-plcheck fpgagpud zaccel-server zynq-agent nbd-server | tr '\\n' ' '; echo")
run("grep -E 'fclk|clock' /sys/kernel/debug/clk/clk_summary | head -8")
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
regs = run("python3 /tmp/plx.py regs") or ""
m = re.search(r"FPGA0_THR_CNT 0x([0-9a-f]{8})", regs)
gated = bool(m and int(m.group(1), 16) & 1)
loaded = "fpgagpu.pl_loaded=1" in (run("cat /proc/cmdline", show=False) or "")
note(f"  FCLK0 gate bit {'SET (clock stopped)' if gated else 'clear'}; PL loaded at boot: {loaded}")


def guarded_read(addr, label):
    """read a PL register in a background process; report a hang instead of hanging this session"""
    out = run("(python3 /tmp/plx.py rdtimed %s > /tmp/rd.out 2>&1 &) ; sleep 5; cat /tmp/rd.out || true" % addr, 60)
    if out is None:
        note(f"  {label}: the board stopped answering after the read was issued")
        return None
    if "PLX rdtimed" not in out:
        note(f"  {label}: the read has NOT returned after 5 s (a normal read takes microseconds)")
    return out


# ---- 2.
refused = False
if other and not A.ignore_guard:
    note("the guard found a problem other than FCLK0 -- the PL is NOT read (it could hang the board for a "
         "reason this experiment does not test):")
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
    note("steps 2-5 skipped: the guard's problems above must be understood first")
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
    if not A.no_stall and ncpu.isdigit() and int(ncpu) < 2:
        note(f"step 5 skipped: {ncpu} CPU -- a stuck read would freeze the only core with nothing left to release it")
    elif not A.no_stall:
        note("step 5: the stall test -- the PL read with the gate set, released from the other CPU after 3 s")
        run("sync; echo synced")
        # everything but the reader stays on CPU 0: CPU 1 is the one expected to stall
        out = run("rm -f /tmp/stall.out; taskset -c 0 sh -c 'python3 /tmp/plx.py gate 1; "
                  "(taskset -c 1 python3 /tmp/plx.py rdtimed 0x40000004 > /tmp/stall.out 2>&1 &); sleep 3; "
                  "echo \"PLX stall after 3 s with the gate set: [$(cat /tmp/stall.out 2>/dev/null)]\"; "
                  "python3 /tmp/plx.py gate 0; sleep 2; echo \"PLX stall after the gate was cleared: [$(cat /tmp/stall.out)]\"'", 90)
        if out is None:
            note("  the board stopped answering during the stall test: the stuck read took the whole system down; "
                 "power-cycle it (it boots the same card)")
    # ---- 6.
    note("step 6: restore")
    run("python3 /tmp/plx.py gate 0; python3 /tmp/plx.py regs; python3 /tmp/plx.py rd 0x40000000")
    run("systemctl start fpgagpud zaccel-server zynq-agent; sleep 2; systemctl is-active fpgagpud zaccel-server zynq-agent | tr '\\n' ' '; echo")
    run("dmesg | grep -i -E 'rcu|stall|lockup|bus|abort' | tail -6")
# ---- the verdict, computed from the numbers above
allsum = open(SUM, encoding="utf-8").read()
mhz = [float(x) for x in re.findall(r"PLX measure \d+ ticks in [0-9.]+ s = ([0-9.]+) MHz", allsum)]
gt = re.findall(r"gate held ([0-9.]+) s .*? counter stopped for (-?[0-9.]+) s; counter afterwards ([0-9.]+) MHz", allsum)
st = re.findall(r"PLX stall after 3 s with the gate set: \[(.*?)\]", allsum)
sr = re.findall(r"PLX stall after the gate was cleared: \[(.*?)\]", allsum)
if mhz:
    note(f"VERDICT FCLK0 on the silicon: {', '.join('%.3f' % m for m in mhz)} MHz measured with the fabric's own counter")
for held, stopped, after in gt:
    held, stopped, after = float(held), float(stopped), float(after)
    if after < 1:
        note("VERDICT gate test: no conclusion (the counter does not run)")
    elif abs(stopped - held) < 0.1:
        note(f"VERDICT gate test: the counter stopped for {stopped:.3f} s while the gate bit was held {held:.3f} s -- "
             "the bit Linux sets at 'Disabling unused clocks' STOPS FCLK0")
    elif abs(stopped) < 0.05:
        note(f"VERDICT gate test: the counter kept running while the gate bit was held {held:.3f} s -- the bit does NOT "
             "stop FCLK0, and the gating explanation of 2026-10-05 is WRONG")
    else:
        note(f"VERDICT gate test: counter short by {stopped:.3f} s for a {held:.3f} s gate -- partial, not understood")
if st and sr:
    hung = "rdtimed" not in st[0]
    secs = re.search(r"in ([0-9.]+) s", sr[0])
    note("VERDICT stall test: the PL read " + ("did NOT return while the gate was set" if hung else "returned while the gate was set")
         + ("; after the gate was cleared it returned, having taken %s s" % secs.group(1) if secs else "; it never returned"))
lines = [l.split("  ", 1)[1] for l in open(SUM, encoding="utf-8").read().splitlines() if "PLX " in l or "boot:" in l or "VERDICT" in l]
run("mkdir -p /boot/reports; cat >> /boot/reports/experiment.txt <<'EOF'\n== experiment " + time.strftime("%Y-%m-%d %H:%M (PC time)")
    + "\n" + "\n".join(lines) + "\nEOF\nsync", show=False)
note(f"done; console log and this summary in {OUT}")
stop[0] = True
