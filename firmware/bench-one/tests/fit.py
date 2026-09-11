"""What is the largest model this hardware can HOLD, and what does that cost per litre and watt.

Speed is deliberately not the metric here. The claim being tested is capacity for the physical size,
which is the one place a box of cheap boards can beat a graphics card outright: a GPU that cannot
hold a model does not run it slowly, it does not run it at all.
"""
CHIP_MB, CHIP_USD = 8, 1.75

# name: (count, MB each, USD each, watts each, cm3 each)
INV = {
    "teensy":  (9,   16,  32, 0.5,  12),
    "esp32s3": (15,   8,   8, 0.8,  10),
    "luckfox": (10,  13,  10, 0.5,   6),
    "zynq":    (2, 1024, 100, 4.0, 250),
}

def show(label, mb, usd, w, cc):
    gb = mb / 1024.0
    print("  %-34s %8.2f GB %8.1f B params  $%7.0f %6.0f W %7.1f L  $%6.2f/GB" %
          (label, gb, gb * 2.0, usd, w, cc / 1000.0, usd / max(gb, 1e-9)))

base_mb  = sum(c * mb for c, mb, _, _, _ in INV.values())
base_usd = sum(c * u  for c, _, u, _, _ in INV.values())
base_w   = sum(c * w  for c, _, _, w, _ in INV.values())
base_cc  = sum(c * v  for c, _, _, _, v in INV.values())

print()
print("  %-34s %11s %17s %9s %7s %8s %11s" %
      ("configuration", "capacity", "INT4 params", "cost", "power", "volume", "cost/GB"))
print("  " + "-" * 104)
show("what you own today", base_mb, base_usd, base_w, base_cc)

# 10 chips on each Teensy and Luckfox
n10 = (INV["teensy"][0] + INV["luckfox"][0]) * 10
show("+10 PSRAM per Teensy and Luckfox",
     base_mb + n10 * CHIP_MB, base_usd + n10 * CHIP_USD, base_w + n10 * 0.05, base_cc + n10 * 1.5)

# the 500 chips being considered
for n in (500, 1000, 2000):
    show("+%d PSRAM chips" % n,
         base_mb + n * CHIP_MB, base_usd + n * CHIP_USD, base_w + n * 0.05, base_cc + n * 1.5)

# the alternative: DIMMs on FPGA
for d in (2, 4, 8):
    show("+%d x 32 GB DIMM on FPGA" % d,
         base_mb + d * 32768, base_usd + d * (60 + 100), base_w + d * 8.0, base_cc + d * 300)

print()
print("  for comparison, what a graphics card holds:")
print("  %-34s %8.2f GB %8.1f B params  $%7.0f %6.0f W %7.1f L" % ("RTX 4090", 24.0, 48.0, 1800, 450, 3.5))
print("  %-34s %8.2f GB %8.1f B params  $%7.0f %6.0f W %7.1f L" % ("RTX 3060 12GB", 12.0, 24.0, 280, 170, 2.0))
print("  %-34s %8.2f GB %8.1f B params  $%7.0f %6.0f W %7.1f L" % ("H100 80GB", 80.0, 160.0, 30000, 700, 2.5))
print()
print("  cost per GB of model capacity:")
print("    PSRAM chip     $%.2f/GB" % (CHIP_USD / (CHIP_MB / 1024.0)))
print("    DDR4 DIMM      $%.2f/GB" % (60.0 / 32.0))
print("    RTX 4090 VRAM  $%.2f/GB" % (1800.0 / 24.0))
print()
