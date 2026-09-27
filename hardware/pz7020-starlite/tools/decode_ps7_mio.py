#!/usr/bin/env python3
"""decode_ps7_mio.py [ps7_init_gpl.c] -- what the PS configuration does to every MIO pin and the PS clocks,
decoded from the register writes in the silicon-3.0 tables, for checking against the vendor manual.

MIO_PIN_xx (0xF8000700 + 4*xx, UG585 B.28): [0] TRI_ENABLE, [1] L0_SEL, [2] L1_SEL, [4:3] L2_SEL, [7:5] L3_SEL,
[8] SPEED, [11:9] IO_TYPE (1 LVCMOS18, 2 LVCMOS25, 3 LVCMOS33, 4 HSTL), [12] PULLUP, [13] DisableRcvr.
"""
import re
import sys

p = sys.argv[1] if len(sys.argv) > 1 else r"D:\espicpc\hardware\pz7020-starlite\ps7\ps7_init_gpl.c"
src = open(p, encoding="utf-8", errors="replace").read()
regs = {}
for name in ("pll", "clock", "ddr", "mio", "peripherals"):
    m = re.search(r"unsigned long ps7_%s_init_data_3_0\[\] = \{(.*?)\n\};" % name, src, re.S)
    for addr, mask, val in re.findall(r"EMIT_MASKWRITE\((0X[0-9A-F]+),\s*(0x[0-9A-F]+)U\s*,\s*(0x[0-9A-F]+)U\)", m.group(1)):
        a, mk, v = int(addr, 16), int(mask, 16), int(val, 16)
        regs[a] = (regs.get(a, 0) & ~mk) | (v & mk)

IOT = {1: "LVCMOS18", 2: "LVCMOS25", 3: "LVCMOS33", 4: "HSTL"}
# the functions the manual assigns (Parts 3.5, 3.6, 3.10, 3.11, 3.12), with the mux code each needs
FUNC = {}
for n in range(1, 7): FUNC[n] = ("qspi0", 0x2)           # L0_SEL
for n in range(16, 28): FUNC[n] = ("enet0", 0x2)         # L0_SEL
for n in range(28, 40): FUNC[n] = ("usb0", 0x4)          # L1_SEL
FUNC[52] = ("mdio0", 0x80); FUNC[53] = ("mdio0", 0x80)  # L3_SEL = 4
for n in range(40, 46): FUNC[n] = ("sdio0", 0x80)        # L3_SEL = 4
FUNC[10] = ("uart0 rx", 0xE0); FUNC[11] = ("uart0 tx", 0xE0)  # L3_SEL = 7
bad = 0
for n in range(54):
    r = regs.get(0xF8000700 + 4 * n)
    if r is None:
        print(f"MIO{n:2d}: not written"); continue
    mux = r & 0xFE
    io = IOT.get((r >> 9) & 7, "?")
    want = FUNC.get(n)
    tag = ""
    if want:
        ok = mux == want[1]
        tag = f"{want[0]:9s} {'OK' if ok else 'WRONG mux 0x%02x want 0x%02x' % (mux, want[1])}"
        bad += not ok
    bank = "500" if n < 16 else "501"
    print(f"MIO{n:2d} bank {bank} {io:8s} mux 0x{mux:02x} pullup {(r >> 12) & 1}  {tag}")
print(f"\nMIO pins with the wrong function for the manual's map: {bad}")

# clocks (UG585 25.10): PLL FDIV from 0xF8000100/104/108 [18:12]; ARM_CLK_CTRL 0x120; UART_CLK_CTRL 0x154;
# SDIO_CLK_CTRL 0x150; FPGA0_CLK_CTRL 0x170; DDR_CLK_CTRL 0x124
PS_CLK = 33.333333
fdiv = {n: (regs.get(a, 0) >> 12) & 0x7F for n, a in (("ARM", 0xF8000100), ("DDR", 0xF8000104), ("IO", 0xF8000108))}
pll = {k: PS_CLK * v for k, v in fdiv.items()}
print("\nPLLs: " + ", ".join(f"{k} {v:.1f} MHz (FDIV {fdiv[k]})" for k, v in pll.items()))
src_name = {0: "IO", 1: "IO", 2: "ARM", 3: "DDR"}


def div_clk(reg, name, srcsh=4, d0sh=8, d1sh=None):
    r = regs.get(reg)
    if r is None:
        print(f"{name}: not written"); return
    s = (r >> srcsh) & 3
    sp = {0: "IO", 1: "IO", 2: "ARM", 3: "DDR"}[s]
    d0 = (r >> d0sh) & 0x3F
    d1 = ((r >> d1sh) & 0x3F) if d1sh else 1
    print(f"{name}: {pll[sp] / max(d0, 1) / max(d1, 1):.2f} MHz  (src {sp} PLL, div {d0}" + (f" x {d1}" if d1sh else "") + ")")


arm = regs.get(0xF8000120, 0)
print(f"CPU: {pll[{0: 'ARM', 1: 'ARM', 2: 'DDR', 3: 'IO'}[(arm >> 4) & 3]] / ((arm >> 8) & 0x3F):.2f} MHz (ARM_CLK_CTRL 0x{arm:08x})")
ddr = regs.get(0xF8000124, 0)
print(f"DDR: {pll['DDR'] / ((ddr >> 20) & 0x3F):.2f} MHz (DDR_3XCLK div {(ddr >> 20) & 0x3F})")
div_clk(0xF8000154, "UART ref clock")
div_clk(0xF8000150, "SDIO ref clock")
div_clk(0xF8000170, "FCLK0 (PL clock 0)", d1sh=20)
div_clk(0xF8000180, "FCLK1 (PL clock 1)", d1sh=20)
print(f"LVL_SHFTR_EN 0xF8000900 = 0x{regs.get(0xF8000900, 0):x}  (0xF: PS<->PL level shifters on)")
print(f"FPGA_RST_CTRL 0xF8000240 = 0x{regs.get(0xF8000240, 0):x}  (0: PL resets released)")
