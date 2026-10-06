#!/usr/bin/env python3
"""check_xsa.py [system.xsa] -- what the BUILT design is, from Vivado's own hardware handoff (system.hwh
inside the .xsa), checked against what the board software assumes:
  * pl_regs (module 'regs'): CLK_HZ = 100 MHz, clocked by the PS7's FCLK_CLK0, at 0x4000_0000;
  * the GP0 interconnect and the PS7's own M_AXI_GP0_ACLK on FCLK_CLK0 -- so with FCLK0 stopped no
    register access to the fabric can complete (zynq-plcheck, plx.py);
  * the address map the drivers use: pl_regs 0x40000000, DMA 0x40400000, GPU 0x43C00000;
  * the bitstream inside the .xsa is the one in this directory (and on the card, linux/out/pl.bit).
    python hardware/pz7020-starlite/vivado/check_xsa.py
"""
import hashlib, io, os, sys, zipfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
xsa = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "system.xsa")
z = zipfile.ZipFile(xsa)
root = ET.parse(io.BytesIO(z.read("system.hwh"))).getroot()
mods = {m.get("INSTANCE"): m for m in root.iter("MODULE")}
fails = 0


def check(what, ok, got=""):
    global fails
    print(("  PASS  " if ok else "  FAIL  ") + what + (f"  ({got})" if got else ""))
    fails += 0 if ok else 1


def param(mod, name):
    return next((p.get("VALUE") for p in mods[mod].iter("PARAMETER") if p.get("NAME") == name), None)


def sig(mod, port):
    return next((p.get("SIGNAME") for p in mods[mod].iter("PORT") if p.get("NAME") == port), None)


check("pl_regs CLK_HZ = 100000000", param("regs", "CLK_HZ") == "100000000", param("regs", "CLK_HZ"))
check("pl_regs aclk is FCLK_CLK0", sig("regs", "aclk") == "ps7_FCLK_CLK0", sig("regs", "aclk"))
for port in ("ACLK", "S00_ACLK", "M00_ACLK", "M01_ACLK"):
    check(f"GP0 interconnect {port} is FCLK_CLK0", sig("gp0_ic", port) == "ps7_FCLK_CLK0", sig("gp0_ic", port))
check("PS7 M_AXI_GP0_ACLK is FCLK_CLK0", sig("ps7", "M_AXI_GP0_ACLK") == "ps7_FCLK_CLK0", sig("ps7", "M_AXI_GP0_ACLK"))
check("FCLK0 enabled at 100 MHz (actual)", param("ps7", "PCW_FPGA_FCLK0_ENABLE") == "1"
      and float(param("ps7", "PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ") or 0) == 100.0,
      f"enable {param('ps7', 'PCW_FPGA_FCLK0_ENABLE')}, {param('ps7', 'PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ')} MHz")
# the bitstream's PS7 settings do not configure the clocks at boot: the SPL's ps7_init does. Its final
# FPGA0_CLK_CTRL value must give the same source and dividers the design was built for.
src, d0, d1 = (param("ps7", k) for k in ("PCW_FCLK0_PERIPHERAL_CLKSRC", "PCW_FCLK0_PERIPHERAL_DIVISOR0", "PCW_FCLK0_PERIPHERAL_DIVISOR1"))
import re
ps7c = open(os.path.join(HERE, "..", "ps7", "ps7_init_gpl.c"), encoding="utf-8", errors="replace").read()
body = re.search(r"unsigned long ps7_clock_init_data_3_0\[\] = \{(.*?)\n\};", ps7c, re.S).group(1)
m = re.search(r"EMIT_MASKWRITE\(0XF8000170,\s*(0x[0-9A-F]+)U\s*,\s*(0x[0-9A-F]+)U\)", body)
v = int(m.group(2), 16)
spl = (("IO PLL", "IO PLL", "ARM PLL", "DDR PLL")[(v >> 4) & 3], (v >> 8) & 0x3F, (v >> 20) & 0x3F)
check("the SPL's FPGA0_CLK_CTRL matches the design (source, divisor0, divisor1)",
      spl == (src, int(d0), int(d1)), f"SPL 0x{v:08x} = {spl}, design = {(src, d0, d1)}")
amap = {mm.get("INSTANCE"): mm.get("BASEVALUE").lower() for mm in mods["ps7"].iter("MEMRANGE")}
for inst, base in (("regs", "0x40000000"), ("dma", "0x40400000"), ("M_AXI_GPU", "0x43c00000")):
    check(f"{inst} at {base}", amap.get(inst) == base, amap.get(inst))
bit = hashlib.sha256(z.read("system.bit")).hexdigest()
for path in (os.path.join(HERE, "build", "system.bit"), os.path.join(HERE, "..", "linux", "out", "pl.bit")):
    if os.path.exists(path):
        h = hashlib.sha256(open(path, "rb").read()).hexdigest()
        check(f"bitstream in the .xsa == {os.path.relpath(path, os.path.join(HERE, '..'))}", h == bit, h[:16])
print("XSA CHECK: PASS" if not fails else f"XSA CHECK: FAIL ({fails})")
sys.exit(1 if fails else 0)
