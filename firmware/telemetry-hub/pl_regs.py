#!/usr/bin/env python3
"""
pl_regs.py -- Linux-side access to the PL register file (hardware/pz7020-starlite/ps7-axi/pl_regs.v)
through /dev/mem on the Zynq PS. Base 0x4000_0000 = the PS7's M_AXI_GP0 window.

    python3 pl_regs.py                 # dump every register
    python3 pl_regs.py led 1           # LED2 on (bit0); 'led 3' = LED2 on + heartbeat on
    python3 pl_regs.py fan 75          # fan duty percent
    python3 pl_regs.py time            # 64-bit fabric time, seconds
    python3 pl_regs.py --selftest      # runs against a fake register window, no hardware

The ID register is checked before anything else is believed: a bitstream that is not this design
(or no bitstream) makes every other read meaningless, so the class refuses to report them.
"""
import mmap
import os
import struct
import sys
import time

BASE = 0x4000_0000
SPAN = 0x1000
DEVCFG = 0xF800_7000          # PS device-configuration block (PCAP); INT_STS at +0x0C


def pl_configured():
    """True when the fabric holds a configuration (DEVCFG INT_STS.PCFG_DONE). False if unreadable."""
    try:
        fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
        try:
            m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=DEVCFG)
            done = bool(struct.unpack_from("<I", m, 0x0C)[0] & 0x4)
            m.close()
            return done
        finally:
            os.close(fd)
    except Exception:                      # noqa: BLE001 - not a Zynq / no /dev/mem rights
        return False
ID_EXPECTED = 0x5A70_2001
REGS = {"ID": 0x00, "TIME_LO": 0x04, "TIME_HI": 0x08, "LED": 0x0C, "KEY": 0x10,
        "FAN_DUTY": 0x14, "FAN_RPM": 0x18, "SCRATCH": 0x1C, "CLK_HZ": 0x20}


class PlRegs:
    """Word access to the register window. `window` is any object with a buffer protocol of
    SPAN bytes (an mmap of /dev/mem on the board, a bytearray in the selftest)."""

    def __init__(self, window):
        self.w = window

    @classmethod
    def open(cls, base=BASE):
        # Touching M_AXI_GP0 with no bitstream behind it is a bus error that kills the process, so
        # ask the PS first: DEVCFG INT_STS (0xF800_700C) bit 2 = PCFG_DONE, a PS register, always safe.
        if not pl_configured():
            raise RuntimeError("PL not configured (PCFG_DONE clear): no bitstream loaded")
        fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        m = mmap.mmap(fd, SPAN, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)
        os.close(fd)
        return cls(m)

    def rd(self, off):
        return struct.unpack_from("<I", self.w, off)[0]

    def wr(self, off, val):
        struct.pack_into("<I", self.w, off, val & 0xFFFF_FFFF)

    def present(self):
        return self.rd(REGS["ID"]) == ID_EXPECTED

    def time_ticks(self):
        lo = self.rd(REGS["TIME_LO"])       # LO first: the hardware latches HI on this read
        hi = self.rd(REGS["TIME_HI"])
        return (hi << 32) | lo

    def time_s(self):
        return self.time_ticks() / self.rd(REGS["CLK_HZ"])

    def dump(self):
        return {k: self.rd(v) for k, v in REGS.items()}

    def set_led(self, bits):
        self.wr(REGS["LED"], bits & 3)

    def set_fan(self, pct):
        self.wr(REGS["FAN_DUTY"], max(0, min(100, int(pct))))


# ============================================================
#  selftest -- a software model of pl_regs.v behind the same API
# ============================================================
class FakeWindow(bytearray):
    """bytearray that behaves like the hardware: ID/CLK_HZ fixed, TIME counts, HI latches on LO."""

    def __init__(self):
        super().__init__(SPAN)
        struct.pack_into("<I", self, REGS["ID"], ID_EXPECTED)
        struct.pack_into("<I", self, REGS["CLK_HZ"], 100_000_000)
        struct.pack_into("<I", self, REGS["LED"], 2)
        struct.pack_into("<I", self, REGS["FAN_DUTY"], 60)
        self.t0 = time.perf_counter()

    def tick(self):
        t = int((time.perf_counter() - self.t0) * 100_000_000) + 0xFFFF_FFF0   # start near a carry
        struct.pack_into("<I", self, REGS["TIME_LO"], t & 0xFFFF_FFFF)
        struct.pack_into("<I", self, REGS["TIME_HI"], t >> 32)


def selftest():
    ok = True
    fw = FakeWindow(); fw.tick()
    r = PlRegs(fw)
    print("1) identity gate")
    if not r.present():
        print("   FAIL: ID not recognised"); ok = False
    else:
        print(f"   ok: ID {r.rd(0):#010x}")
    bad = PlRegs(bytearray(SPAN))
    if bad.present():
        print("   FAIL: empty window accepted"); ok = False
    else:
        print("   ok: empty/foreign bitstream rejected")

    print("2) 64-bit time across the carry, converted with the hardware's own CLK_HZ")
    t1 = r.time_ticks(); fw.t0 -= 0.5; fw.tick(); t2 = r.time_ticks()
    if t2 <= t1 or (t2 >> 32) < 1:
        print(f"   FAIL: {t1} -> {t2}"); ok = False
    else:
        print(f"   ok: {t1} -> {t2} ticks = {r.time_s():.3f} s")

    print("3) writes land where the map says")
    r.set_led(1); r.set_fan(250)
    d = r.dump()
    if d["LED"] != 1 or d["FAN_DUTY"] != 100:
        print(f"   FAIL: {d}"); ok = False
    else:
        print(f"   ok: LED={d['LED']} FAN_DUTY={d['FAN_DUTY']} (clamped)")

    print("\nPASSED" if ok else "\nFAILED")
    return ok


def main(argv):
    if "--selftest" in argv:
        sys.exit(0 if selftest() else 1)
    r = PlRegs.open()
    if not r.present():
        print(f"pl_regs: ID {r.rd(0):#010x} != {ID_EXPECTED:#010x} -- the PL is not running pz7020_ps7_top")
        sys.exit(2)
    if len(argv) >= 3 and argv[1] == "led":
        r.set_led(int(argv[2], 0))
    elif len(argv) >= 3 and argv[1] == "fan":
        r.set_fan(int(argv[2]))
    elif len(argv) >= 2 and argv[1] == "time":
        print(f"{r.time_s():.6f}")
        return
    for k, v in r.dump().items():
        print(f"{k:9s} {v:#010x}  {v}")


if __name__ == "__main__":
    main(sys.argv)
