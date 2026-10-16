#!/bin/bash
# qemu_spl_chain_test.sh -- run the real boot chain FROM THE SPL ONWARD on QEMU xilinx-zynq-a9:
#   a test-only SPL (copy of the U-Boot tree + two QEMU patches) loads the PRODUCTION u-boot.img,
#   which runs the production boot.scr, pl.bit, zImage, DTB and rootfs -- all read from the
#   production SD image. This is the stage qemu_test.sh skips (it hands QEMU the kernel directly),
#   which is how an SPL with no console/MMC node reached the card on 2026-09-24.
#   The two patches, and why: QEMU 6.2 has no DDR controller or PLL model, so ps7_init() would poll
#   forever -> replaced by direct writes of the clock registers' FINAL values from the same
#   ps7_init_gpl.c (as qemu_uboot_test.sh's stub does). Without them QEMU's UART is unclocked and drops
#   every character ("uart_event: uart is unclocked or in reset" in -d guest_errors): the chain ran
#   silent for 600 s on 2026-10-05. UART_CLK_CTRL is NOT written (the board gates UART1; QEMU 6.2 then
#   divides by 0 Hz and dies). QEMU's SLCR reports boot mode JTAG -> spl_boot_device() forced to SD.
#   NOT covered: the BootROM loading boot.bin, ps7_init (clocks, DDR training) -- hardware only.
#     wsl -d Ubuntu-22.04 -u root --exec bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_spl_chain_test.sh
set -uo pipefail
# QEMU wants an SD card whose size is a power of two: round the copy UP to one, never down (a fixed
# "truncate -s 2G" cut the 4.2 GB machine image inside its root filesystem -> "VFS: Unable to mount root")
sd_pow2() { local s p=1; s=$(stat -c %s "$1"); while [ "$p" -lt "$s" ]; do p=$((p * 2)); done; truncate -s "$p" "$1"; }
SRC=/root/zynq/u-boot; T=/root/zynq/u-boot-qemutest; IMG=/root/zynq/pz7020-starlite-sd.img
LOG=/root/zynq/qemu-spl-chain.log; TIMEOUT=${TIMEOUT:-600}
rm -rf "$T"; mkdir -p "$T"; (cd "$SRC" && tar --exclude=.git -cf - .) | (cd "$T" && tar -xf -); cd "$T"
python3 - <<'PY'
import re
# the final value of each clock register after ps7_init's silicon-3.0 pll + clock tables (same parse as
# qemu_uboot_test.sh), from the ps7_init_gpl.c this SPL is built with
src = open("board/xilinx/zynq/zynq-pz7020-starlite/ps7_init_gpl.c", encoding="utf-8", errors="replace").read()
regs = {}
for name in ("pll", "clock"):
    body = re.search(r"unsigned long ps7_%s_init_data_3_0\[\] = \{(.*?)\n\};" % name, src, re.S).group(1)
    for op in re.finditer(r"EMIT_(MASKWRITE|WRITE)\((0X[0-9A-F]+),\s*(0x[0-9A-F]+)U\s*(?:,\s*(0x[0-9A-F]+)U)?\)", body):
        a = int(op.group(2), 16)
        if op.group(1) == "WRITE":
            regs[a] = int(op.group(3), 16)
        else:
            mk, v = int(op.group(3), 16), int(op.group(4), 16)
            regs[a] = (regs.get(a, 0) & ~mk) | (v & mk)
keep = (0xF8000100, 0xF8000104, 0xF8000108, 0xF8000120, 0xF8000124, 0xF8000150, 0xF8000170, 0xF80001C4)
writes = "".join("\twritel(0x%08X, 0x%08X);\n" % (regs[a], a) for a in keep)
p="arch/arm/mach-zynq/spl.c"; s=open(p).read()
a="#if !defined(CONFIG_DEBUG_UART_BOARD_INIT)\n\tps7_init();\n#endif\n"   # the call board_init_f makes (the other one is debug-UART only)
assert s.count(a)==1
s=s.replace(a,"\t/* QEMU TEST BUILD: no DDRC/PLL model in QEMU, so ps7_init() (which polls them) is skipped and\n"
              "\t   the board's final clock register values are written directly (UART and SD clocked) */\n"
              "\twritel(0xDF0D, 0xF8000008);\t/* SLCR unlock */\n" + writes +
              "\twritel(0x767B, 0xF8000004);\t/* SLCR lock */\n")
s=s.replace("#include <asm/arch/ps7_init_gpl.h>\n", "#include <asm/arch/ps7_init_gpl.h>\n#include <asm/io.h>\n", 1)
print("board clock writes:", " ".join("0x%08X=0x%X" % (a, regs[a]) for a in keep))
a="u32 spl_boot_device(void)\n{\n\tu32 mode;\n"
assert s.count(a)==1; s=s.replace(a, a+"\n\treturn BOOT_DEVICE_MMC1;\t/* QEMU TEST BUILD: QEMU's SLCR reports JTAG */\n")
open(p,"w").write(s); print("test patches applied to", p)
PY
[ $? -eq 0 ] || { echo "SPL CHAIN: test patches did not apply -- NOT running an unpatched SPL"; exit 1; }
export CROSS_COMPILE=arm-linux-gnueabihf- ARCH=arm
make -j"$(nproc)" DEVICE_TREE=zynq-pz7020-starlite 2>&1 | grep -E "error|Error" ; ls -la spl/u-boot-spl
cp "$IMG" /root/zynq/qemu-chain-sd.img && sd_pow2 /root/zynq/qemu-chain-sd.img
echo "production image: $(sha256sum "$IMG" | cut -c1-16)...  QEMU start $(date +%T)"
timeout "$TIMEOUT" qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -serial mon:stdio \
  -device loader,file=spl/u-boot-spl-dtb.bin,addr=0x0,force-raw=on \
  -drive file=/root/zynq/qemu-chain-sd.img,if=sd,format=raw </dev/null >"$LOG" 2>&1
# (the SPL as the BootROM gives it: the binary WITH its appended device tree, CONFIG_OF_SEPARATE. The first
#  version loaded the ELF with -kernel, which carries no device tree: spl_init() failed and the SPL sat in
#  hang() without a word -- found 2026-10-06 from QEMU's monitor: PC in hang(), called from board_init_r
#  right after spl_init(). No cpu-num loader: QEMU rejects addr=0 there, and the A9 starts at its reset
#  vector, 0x0, where the binary is)
echo "QEMU end $(date +%T)"
grep -a -n -E "U-Boot SPL|Trying to boot|uImage|^U-Boot 20|DRAM:|^MMC:|Found U-Boot script|Loading PL|Starting kernel|Booting Linux|EXT4-fs \(mmcblk0p2\): mounted|automatic login|ZYNQ-REPORT|Kernel panic|### ERROR|resetting|FAIL" "$LOG" | cut -c1-160 | head -60
grep -a -q "ZYNQ-REPORT END" "$LOG" && { echo "SPL CHAIN: SPL -> U-Boot -> boot.scr -> Linux -> autologin -> report, all reached"; exit 0; }
# (the first version ended with the failure branch's tail, so a failed chain still exited 0)
echo "SPL CHAIN: did not reach the report"; tail -25 "$LOG" | cut -c1-160; exit 1
