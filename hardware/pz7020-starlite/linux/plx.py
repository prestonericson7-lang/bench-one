#!/usr/bin/env python3
"""plx.py -- PL/SLCR register tool for the FCLK0 experiment on the PZ7020 (runs on the board, as root).

  regs                 SLCR FPGA0 clock/throttle, FPGA_RST_CTRL, LVL_SHFTR_EN, DEVCFG INT_STS (PS registers)
  rd ADDR              one 32-bit read (ADDR in the PL window goes through M_AXI_GP0)
  rdtimed ADDR [FILE]  the same, printing how long the read itself took and when it finished; FILE gets the
                       moment it was issued (CLOCK_MONOTONIC, shared by every process on the board)
  gate 0|1             FCLK0's gate bit, FPGA0_THR_CNT bit 0 (the bit drivers/clk/zynq/clkc.c gates with),
                       and the moment it was written
  measure SECS         pl_regs TIME (a 64-bit counter on FCLK0) over SECS of CPU time: FCLK0's frequency
  lockstate            SLCR_LOCKSTA: Linux leaves the SLCR unlocked; every write here restores the lock as found
  gatetest SECS        TIME read, gate set for SECS, gate cleared, TIME read: no PL access while gated.
                       If the gate stops FCLK0 the counter falls SECS short of the wall clock, counted at
                       the rate the counter itself runs at just before and after.
Every number printed is read from the hardware; nothing is assumed.
"""
import mmap, os, struct, sys, time

SLCR, DEVCFG, REGS = 0xF8000000, 0xF8007000, 0x40000000
THR_CNT, UNLOCK, LOCK, LOCKSTA = 0x178, 0x008, 0x004, 0x00C
_maps = {}


def win(base):
    if base not in _maps:
        fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        _maps[base] = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)
        os.close(fd)
    return _maps[base]


def rd(a):
    return struct.unpack_from("<I", win(a & ~0xFFF), a & 0xFFF)[0]


def wr(a, v):
    struct.pack_into("<I", win(a & ~0xFFF), a & 0xFFF, v & 0xFFFFFFFF)


# PLX_FAKE: a test backend on the PC instead of /dev/mem, so this script's own arithmetic is checked against a
# counter that moves. 'gate' = the gate bit stops the 100 MHz counter (the hypothesis), 'nogate' = it does
# not (the hypothesis wrong), 'dead' = the counter never moves (no PL). The SLCR honours its lock as the
# silicon does: writes are ignored while locked. PLX_FAKE_MHZ: the counter's real rate, while CLK_HZ still
# says the design's 100 MHz (a board whose FCLK0 is not what the design was built for).
FAKE = os.environ.get("PLX_FAKE")
FAKE_HZ = float(os.environ.get("PLX_FAKE_MHZ", "100")) * 1e6
if FAKE:
    _r = {SLCR + LOCKSTA: 1 if os.environ.get("PLX_FAKE_LOCKED") else 0, SLCR + THR_CNT: 0, SLCR + 0x17C: 0x00010000, SLCR + 0x900: 0xF, SLCR + 0x240: 0,
          SLCR + 0x108: 0x0001E000, SLCR + 0x170: 0x00200500, SLCR + 0x174: 0, DEVCFG + 0x0C: 0,
          REGS: 0x5A702001, REGS + 0x20: 100_000_000}
    _t = {"run": 0.0, "since": time.monotonic(), "hi": 0}

    def _count():
        now = time.monotonic()
        running = FAKE == "nogate" or not _r[SLCR + THR_CNT] & 1
        if running and FAKE != "dead":
            _t["run"] += now - _t["since"]
        _t["since"] = now
        return int(_t["run"] * FAKE_HZ)

    def win(base):                                # noqa: F811 -- nothing to map on the PC
        return None

    def rd(a):                                    # noqa: F811
        if a == REGS + 0x04:
            c = _count(); _t["hi"] = c >> 32; return c & 0xFFFFFFFF
        if a == REGS + 0x08:
            return _t["hi"]
        if a == SLCR + 0x17C:                     # model: bit 16 follows the gate
            _count(); return 0x00010000 if (FAKE == "nogate" or not _r[SLCR + THR_CNT] & 1) else 0
        return _r.get(a, 0)

    def wr(a, v):                                 # noqa: F811
        _count()
        if a == SLCR + UNLOCK and v == 0xDF0D:
            _r[SLCR + LOCKSTA] = 0
        elif a == SLCR + LOCK and v == 0x767B:
            _r[SLCR + LOCKSTA] = 1
        elif a & ~0xFFF == SLCR and _r[SLCR + LOCKSTA] & 1:
            return                                # locked: ignored, as on the silicon
        else:
            _r[a] = v & 0xFFFFFFFF


def slcr_wr(off, v):
    # leave the SLCR lock as it was: Linux unlocks it once at boot and never again, and its own writes
    # (the clock framework, the restart handler) assume it stays unlocked. Locking it here broke 'reboot'
    # (measured in QEMU: SLCR_LOCKSTA 1 after the first version, then "Reboot failed -- System halted").
    was_locked = rd(SLCR + LOCKSTA) & 1
    wr(SLCR + UNLOCK, 0xDF0D); wr(SLCR + off, v)
    if was_locked:
        wr(SLCR + LOCK, 0x767B)


def gate(on):
    v = rd(SLCR + THR_CNT)
    slcr_wr(THR_CNT, (v | 1) if on else (v & ~1))
    return rd(SLCR + THR_CNT)


def ticks():
    lo = rd(REGS + 0x04)              # reading TIME_LO latches TIME_HI
    hi = rd(REGS + 0x08)
    return (hi << 32) | lo


cmd = sys.argv[1] if len(sys.argv) > 1 else "regs"
if cmd == "regs":
    s = {o: rd(SLCR + o) for o in (0x108, 0x170, 0x174, 0x178, 0x17C, 0x240, 0x900)}
    print("PLX regs IO_PLL_CTRL 0x%08x FPGA0_CLK_CTRL 0x%08x FPGA0_THR_CTRL 0x%08x FPGA0_THR_CNT 0x%08x "
          "FPGA0_THR_STA 0x%08x FPGA_RST_CTRL 0x%08x LVL_SHFTR_EN 0x%08x DEVCFG_INT_STS 0x%08x"
          % (s[0x108], s[0x170], s[0x174], s[0x178], s[0x17C], s[0x240], s[0x900], rd(DEVCFG + 0x0C)))
elif cmd == "lockstate":
    v = rd(SLCR + LOCKSTA)
    print("PLX SLCR_LOCKSTA %d (%s)" % (v, "locked" if v & 1 else "unlocked, as Linux leaves it"))
elif cmd == "rd":
    a = int(sys.argv[2], 0)
    print("PLX rd 0x%08x = 0x%08x" % (a, rd(a)))
elif cmd == "rdtimed":
    a = int(sys.argv[2], 0)
    win(a & ~0xFFF)                   # map first, so only the read itself is timed
    t0 = time.monotonic()
    if len(sys.argv) > 3:             # ISSUEDFILE: when the read was issued, for the stall test's verdict
        with open(sys.argv[3], "w") as f:
            f.write("%.6f" % t0)
        t0 = time.monotonic()
    v = rd(a); t1 = time.monotonic()
    print("PLX rdtimed 0x%08x = 0x%08x in %.6f s, done at %.6f" % (a, v, t1 - t0, t1), flush=True)
elif cmd == "gate":
    v = gate(sys.argv[2] == "1")
    print("PLX gate %s -> FPGA0_THR_CNT 0x%08x at %.6f" % (sys.argv[2], v, time.monotonic()))
elif cmd == "measure":
    secs = float(sys.argv[2])
    w0, k0 = time.monotonic(), ticks()
    time.sleep(secs)
    w1, k1 = time.monotonic(), ticks()
    print("PLX measure %d ticks in %.6f s = %.6f MHz (CLK_HZ register %d)" % (k1 - k0, w1 - w0, (k1 - k0) / (w1 - w0) / 1e6, rd(REGS + 0x20)))
elif cmd == "gatetest":
    secs = float(sys.argv[2])
    # the counter's own rate, before and after: CLK_HZ is only the rate the design was built for, and a 5 %
    # difference on the silicon would move "stopped for" by 0.1 s over the 2 s window -- enough to turn
    # either answer into "partial"
    wa, ka = time.monotonic(), ticks()
    time.sleep(0.5)
    w0, k0 = time.monotonic(), ticks()
    g1 = gate(True); wg1 = time.monotonic()
    sta_gated = rd(SLCR + 0x17C)                  # FPGA0_THR_STA while the gate is held: the PS's own status
    time.sleep(secs)
    wg0 = time.monotonic(); g0 = gate(False)
    sta_after = rd(SLCR + 0x17C)
    print("PLX gatetest FPGA0_THR_STA while gated 0x%08x, after 0x%08x" % (sta_gated, sta_after))
    w1, k1 = time.monotonic(), ticks()
    time.sleep(0.5)                               # the counter must be seen running again afterwards
    w2, k2 = time.monotonic(), ticks()
    before, after = (k0 - ka) / (w0 - wa) / 1e6, (k2 - k1) / (w2 - w1) / 1e6
    # the measured rate when the counter runs; CLK_HZ (0 where there is no PL, as in QEMU) only otherwise
    hz = (before + after) / 2 * 1e6 if before >= 1 and after >= 1 else (rd(REGS + 0x20) or 100_000_000)
    wall, run = w1 - w0, (k1 - k0) / hz
    print("PLX gatetest wall %.6f s, gate held %.6f s (THR_CNT set 0x%08x, cleared 0x%08x), counter advanced %.6f s "
          "at its own %.3f MHz (%.3f before, CLK_HZ %d) -> counter stopped for %.6f s; counter afterwards %.3f MHz"
          % (wall, wg0 - wg1, g1, g0, run, hz / 1e6, before, rd(REGS + 0x20), wall - run, after))
    if after < 1:
        print("PLX gatetest NO CONCLUSION: the counter does not run (no PL, or its clock is not FCLK0)")
else:
    sys.exit(__doc__)
