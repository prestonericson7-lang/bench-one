#!/usr/bin/env python3
"""plx.py -- PL/SLCR register tool for the FCLK0 experiment on the PZ7020 (runs on the board, as root).

  regs                 SLCR FPGA0 clock/throttle, FPGA_RST_CTRL, LVL_SHFTR_EN, DEVCFG INT_STS (PS registers)
  rd ADDR              one 32-bit read (ADDR in the PL window goes through M_AXI_GP0)
  rdtimed ADDR         the same, printing how long the read itself took
  gate 0|1             FCLK0's gate bit, FPGA0_THR_CNT bit 0 (the bit drivers/clk/zynq/clkc.c gates with)
  measure SECS         pl_regs TIME (a 64-bit counter on FCLK0) over SECS of CPU time: FCLK0's frequency
  gatetest SECS        TIME read, gate set for SECS, gate cleared, TIME read: no PL access while gated.
                       If the gate stops FCLK0 the counter falls SECS short of the wall clock.
Every number printed is read from the hardware; nothing is assumed.
"""
import mmap, os, struct, sys, time

SLCR, DEVCFG, REGS = 0xF8000000, 0xF8007000, 0x40000000
THR_CNT, UNLOCK, LOCK = 0x178, 0x008, 0x004
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


def slcr_wr(off, v):
    wr(SLCR + UNLOCK, 0xDF0D); wr(SLCR + off, v); wr(SLCR + LOCK, 0x767B)


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
elif cmd == "rd":
    a = int(sys.argv[2], 0)
    print("PLX rd 0x%08x = 0x%08x" % (a, rd(a)))
elif cmd == "rdtimed":
    a = int(sys.argv[2], 0)
    win(a & ~0xFFF)                   # map first, so only the read itself is timed
    t0 = time.monotonic(); v = rd(a); t1 = time.monotonic()
    print("PLX rdtimed 0x%08x = 0x%08x in %.6f s" % (a, v, t1 - t0), flush=True)
elif cmd == "gate":
    print("PLX gate %s -> FPGA0_THR_CNT 0x%08x" % (sys.argv[2], gate(sys.argv[2] == "1")))
elif cmd == "measure":
    secs = float(sys.argv[2])
    w0, k0 = time.monotonic(), ticks()
    time.sleep(secs)
    w1, k1 = time.monotonic(), ticks()
    print("PLX measure %d ticks in %.6f s = %.6f MHz (CLK_HZ register %d)" % (k1 - k0, w1 - w0, (k1 - k0) / (w1 - w0) / 1e6, rd(REGS + 0x20)))
elif cmd == "gatetest":
    secs = float(sys.argv[2])
    w0, k0 = time.monotonic(), ticks()
    g1 = gate(True); wg1 = time.monotonic()
    time.sleep(secs)
    wg0 = time.monotonic(); g0 = gate(False)
    w1, k1 = time.monotonic(), ticks()
    time.sleep(0.5)                               # the counter must be seen running again afterwards
    w2, k2 = time.monotonic(), ticks()
    hz = rd(REGS + 0x20) or 100_000_000          # CLK_HZ register; 0 only where there is no PL (QEMU)
    wall, run, after = w1 - w0, (k1 - k0) / hz, (k2 - k1) / (w2 - w1) / 1e6
    print("PLX gatetest wall %.6f s, gate held %.6f s (THR_CNT set 0x%08x, cleared 0x%08x), counter advanced %.6f s "
          "-> counter stopped for %.6f s; counter afterwards %.3f MHz" % (wall, wg0 - wg1, g1, g0, run, wall - run, after))
    if after < 1:
        print("PLX gatetest NO CONCLUSION: the counter does not run (no PL, or its clock is not FCLK0)")
else:
    sys.exit(__doc__)
