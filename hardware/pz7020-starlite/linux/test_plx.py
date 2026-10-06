#!/usr/bin/env python3
"""test_plx.py -- plx.py's own logic against its PC test backend (PLX_FAKE), and the experiment driver's
verdicts computed from plx.py's real output. Run on the PC:   python test_plx.py
  * the SLCR lock: a write with the lock unlocked leaves it unlocked (as Linux needs: its restart handler
    writes without unlocking); a write with it locked lands and the lock is restored;
  * measure: ~100 MHz from a 100 MHz counter;
  * gatetest, three worlds: the gate bit stops the counter (the hypothesis) -> 'STOPS FCLK0'; it does not
    -> 'is WRONG'; the counter never moves -> 'no conclusion'; and the first two again with the counter at
    95 MHz while CLK_HZ says 100 (counted at the design's rate, both had come out 'partial');
  * the stall verdict, for a read that hung until the gate was cleared, one that did not hang, one whose
    python started only after the gate was cleared (timed on CLOCK_MONOTONIC: 'no conclusion'), and a board
    that froze whole (the driver then gets no answer at all);
  * the pattern that picks the board's serial port: the SPL's banner, U-Boot's, the kernel's and the
    prompt count; other boards' chatter does not.
"""
import os, runpy, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLX, DRV = os.path.join(HERE, "plx.py"), os.path.join(HERE, "board_experiment.py")
fails = 0


def check(what, ok, got=""):
    global fails
    print(("  PASS  " if ok else "  FAIL  ") + what + (f"  [{got}]" if got and not ok else ""))
    fails += 0 if ok else 1


def plx_ns(mode, locked):
    """plx.py's functions over its fake backend, in this process (runs its 'regs' command once on load)"""
    os.environ["PLX_FAKE"] = mode
    os.environ.pop("PLX_FAKE_LOCKED", None)
    if locked:
        os.environ["PLX_FAKE_LOCKED"] = "1"
    argv, sys.argv = sys.argv, [PLX, "regs"]
    try:
        import contextlib, io
        with contextlib.redirect_stdout(io.StringIO()):
            return runpy.run_path(PLX)
    finally:
        sys.argv = argv


def plx(mode, *args):
    env = dict(os.environ, PLX_FAKE=mode); env.pop("PLX_FAKE_LOCKED", None)
    return subprocess.run([sys.executable, PLX, *args], capture_output=True, text=True, env=env).stdout


def verdict(text):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        f.write(text)
    try:
        return subprocess.run([sys.executable, DRV, "--verdict-only", f.name], capture_output=True, text=True).stdout
    finally:
        os.unlink(f.name)


print("== the SLCR lock")
for locked in (False, True):
    ns = plx_ns("gate", locked)
    ns["gate"](True)
    lock = ns["rd"](ns["SLCR"] + ns["LOCKSTA"]) & 1
    thr = ns["rd"](ns["SLCR"] + ns["THR_CNT"]) & 1
    check(f"start {'locked' if locked else 'unlocked'}: the gate write lands and the lock is left {'locked' if locked else 'unlocked'}",
          thr == 1 and lock == int(locked), f"THR_CNT bit0 {thr}, LOCKSTA {lock}")
    ns["gate"](False)
    check(f"start {'locked' if locked else 'unlocked'}: the gate clears again", ns["rd"](ns["SLCR"] + ns["THR_CNT"]) & 1 == 0)

print("== measure")
m = plx("gate", "measure", "1")
mhz = float(m.split("=")[1].split("MHz")[0]) if "MHz" in m else 0
check("a 100 MHz counter measures 99-101 MHz", 99 <= mhz <= 101, m.strip())

print("== gatetest and the driver's verdicts")
for mode, want in (("gate", "STOPS FCLK0"), ("nogate", "is WRONG"), ("dead", "no conclusion")):
    out = plx(mode, "measure", "1") + plx(mode, "gatetest", "2")
    v = verdict(out)
    check(f"world '{mode}': verdict says '{want}'", want in v, (out + v).strip().replace("\n", " | "))
    if mode == "gate":
        stopped = float(out.split("counter stopped for ")[1].split(" s")[0])
        check("the gate world: the counter stopped 1.9-2.1 s for a 2 s gate", 1.9 <= stopped <= 2.1, stopped)
        check("the gate world: THR_STA read while gated and after, both reported", "FPGA0_THR_STA while gated" in out
              and "FPGA0_THR_STA" in v, out)

# FCLK0 not at the design's 100 MHz (CLK_HZ still says 100): counted at the design's rate, a 95 MHz counter
# falls 0.1 s behind over the window by itself, and both answers came out 'partial'
for mode, want in (("gate", "STOPS FCLK0"), ("nogate", "is WRONG")):
    env = dict(os.environ, PLX_FAKE=mode, PLX_FAKE_MHZ="95"); env.pop("PLX_FAKE_LOCKED", None)
    out = subprocess.run([sys.executable, PLX, "gatetest", "2"], capture_output=True, text=True, env=env).stdout
    v = verdict(out)
    check(f"world '{mode}' with the counter at 95 MHz: verdict says '{want}'", want in v, (out + v).strip().replace("\n", " | "))

print("== the stall verdict")
hung = ("PLX stall after 3 s with the gate set: []\n"
        "PLX stall after the gate was cleared: [PLX rdtimed 0x40000004 = 0x0012d687 in 3.104512 s]\n")
v = verdict(hung)
check("a read that waited for the gate: 'did NOT return while the gate was set' and its time", "did NOT return" in v and "3.104512" in v, v)
fine = ("PLX stall after 3 s with the gate set: [PLX rdtimed 0x40000004 = 0x0012d687 in 0.000004 s]\n"
        "PLX stall after the gate was cleared: [PLX rdtimed 0x40000004 = 0x0012d687 in 0.000004 s]\n")
check("a read that did not wait: 'returned while the gate was set'", "returned while the gate was set" in verdict(fine), verdict(fine))
never = "PLX stall after 3 s with the gate set: []\nPLX stall after the gate was cleared: []\n"
check("a read that never returned: 'it never returned'", "never returned" in verdict(never), verdict(never))
print("== the stall verdict, timed on CLOCK_MONOTONIC")
import re
g = plx("gate", "gate", "1")
check("plx.py's gate line carries the moment of the write", re.search(r"PLX gate 1 -> FPGA0_THR_CNT 0x[0-9a-f]+ at [0-9.]+$", g.strip()), g)
with tempfile.TemporaryDirectory() as td:
    r = plx("gate", "rdtimed", "0x40000004", os.path.join(td, "issued"))
    iss = open(os.path.join(td, "issued")).read()
check("plx.py's rdtimed line: value, duration and end; the issue moment in its file",
      re.search(r"PLX rdtimed 0x40000004 = 0x[0-9a-f]{8} in [0-9.]+ s, done at [0-9.]+$", r.strip()) and re.fullmatch(r"[0-9]+\.[0-9]{6}", iss), r + iss)
EARLY = "02:00:00    | PLX gate 0 -> FPGA0_THR_CNT 0x00000000 at 50.000000\n"     # step 2's, before the stall test
waited = (EARLY + "02:00:05    | PLX gate 1 -> FPGA0_THR_CNT 0x00000001 at 100.000000\n"
          "02:00:08    | PLX stall after 3 s with the gate set: [] issued [100.700000]\n"
          "02:00:09    | PLX gate 0 -> FPGA0_THR_CNT 0x00000000 at 103.600000\n"
          "02:00:11    | PLX stall after the gate was cleared: [PLX rdtimed 0x40000004 = 0x0012d687 in 2.900100 s, done at 103.600100] issued [100.700000]\n")
v = verdict(waited)
check("issued while gated, returned once cleared: 'did NOT return', issued 2.900 s before the clear", "did NOT return while the gate was set (issued 2.900 s before" in v, v)
late = waited.replace("[] issued [100.700000]", "[] issued []").replace("in 2.900100 s, done at 103.600100] issued [100.700000]",
                                                                    "in 0.000004 s, done at 104.100004] issued [104.100000]")
v = verdict(late)
check("python started after the clear: 'no conclusion', not 'did NOT return'", "no conclusion -- the read was issued 0.500 s AFTER" in v
      and "did NOT return" not in v, v)

froze = ("02:00:00    PLX measure 100000000 ticks in 1.000000 s = 100.000000 MHz (CLK_HZ register 100000000)\n"
         "02:00:09    PLX stall froze the board: no console answer for 90 s after CPU 1 read the PL with the gate set\n")
v = verdict(froze)
check("a board that froze in the stall test: 'froze the WHOLE board', and the FCLK0 figure kept", "froze the WHOLE board" in v
      and "100.000 MHz" in v, v)
check("no freeze verdict when the stall test was not run", "froze" not in verdict(hung) and "froze" not in verdict(""), verdict(hung))

print("== what step 1 makes of the guard's log (its own line format, from the QEMU boots)")


def guard(text):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        f.write(text)
    try:
        return subprocess.run([sys.executable, DRV, "--guard-only", f.name], capture_output=True, text=True).stdout.strip()
    finally:
        os.unlink(f.name)


HEAD = ("== boot 2\n[   15.94 s] kernel 6.12.0-dirty; guards: clk_ignore_unused yes, marker fpgagpu.pl_loaded=1 yes\n"
        "[   16.07 s] fclk0 in the clock framework: enable_count 1, prepare_count 1, rate 100000000 Hz\n"
        "[   16.11 s] SLCR IO_PLL_CTRL 0x0001e000 FPGA0_CLK_CTRL 0x00200500 FPGA0_THR_CNT 0x00000000\n"
        "[   16.11 s] DEVCFG INT_STS 0x00000000 STATUS 0x40000a30\n")
R1 = "[   16.13 s] reading 0x40000000 (platform registers) through M_AXI_GP0 -- if this is the last line, that read hung\n"
V1 = "[   16.14 s]   0x40000000 = 0x5a702001 (expected: this design's ID)\n"
R2 = "[   16.15 s] reading 0x43c00000 (GPU registers ('GPU1')) through M_AXI_GP0 -- if this is the last line, that read hung\n"
OLD = "== boot 1\n[ 5.0 s] problem: PS/PL level shifters not all on: LVL_SHFTR_EN 0x0, needs 0xF\n[ 5.1 s] verdict: BLOCKED -- x\n"
for what, text, want in (
        ("both reads came back: nothing stuck", HEAD + R1 + V1 + R2 + "[ 16.2 s]   0x43c00000 = 0x47505531 (expected)\n"
         "[ 16.2 s] verdict: OK -- clock, level shifters and reset fine, and the PL reads came back\n", "other=0 stuck=None"),
        ("the first read never came back", HEAD + R1, "other=0 stuck=0x40000000 platform registers"),
        ("the GPU read never came back (its name has its own parentheses)", HEAD + R1 + V1 + R2,
         "other=0 stuck=0x43c00000 GPU registers ('GPU1')"),
        ("a previous boot's problems are not this boot's", OLD + HEAD + R1 + V1 + R2, "other=0 stuck=0x43c00000 GPU registers ('GPU1')"),
        ("blocked on FCLK0 alone: neither another problem nor a stuck read", HEAD +
         "[ 16.13 s] problem: FCLK0 gated: FPGA0_THR_CNT bit 0 = 1 -- Linux stopped the PL's AXI clock\n"
         "[ 16.19 s] verdict: BLOCKED -- the PL is not read\n", "other=0 stuck=None"),
        ("blocked on the level shifters: one other problem", HEAD + OLD.split("\n", 1)[1], "other=1 stuck=None")):
    got = guard(text)
    check(what, got == want, got)
# as the driver reads it on the board: every line prefixed G|, with other console output (here a systemd status
# line; on silicon more likely the kernel's RCU stall warnings) landing between and on the guard's lines
ANIM = "[  *** ] A start job is running for Check the PL's clock (1min 3s / no limit)"
pref = "".join("G|" + l + "\n" for l in (HEAD + R1).splitlines())
for what, text in (("prefixed, a status line after the stuck read", pref + ANIM + "\n"),
                   ("prefixed, a status line glued onto the stuck read's line", pref.rstrip("\n") + ANIM + "\n" + ANIM + "\n")):
    got = guard(text)
    check(what + ": still stuck at 0x40000000", got == "other=0 stuck=0x40000000 platform registers", got)
v = verdict("02:00:00    PLX guard read stuck: 0x43c00000 (GPU registers ('GPU1')) -- the guard's last line is that read\n")
check("a stuck guard read gets its verdict, with the whole name", "0x43c00000 (GPU registers ('GPU1')) never returned" in v, v)

print("== which serial port is the board")
import re
src = open(DRV, encoding="utf-8").read()
MARK = re.search(r'^MARK = r"(.*)"$', src, re.M).group(1)
for line, want in (("U-Boot SPL 2024.01 (Oct 05 2026 - 20:57:32 +0000)", True),      # the first stage's banner
                   ("U-Boot 2024.01 (Oct 05 2026 - 20:57:32 +0000)", True),
                   ("[    0.000000] Booting Linux on physical CPU 0x0", True), ("root@zynq1:~# ", True),
                   ("STM32H743 tick 1234 U-Boot? no", False), ("hello from an esp32 at 115200", False)):
    check(f"{'board' if want else 'not the board'}: {line[:44]}", bool(re.search(MARK, line)) == want)

print("TEST PLX: PASS" if not fails else f"TEST PLX: FAIL ({fails})")
sys.exit(1 if fails else 0)
