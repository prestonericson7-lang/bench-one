#!/bin/bash
# qemu_spl_chain_test.sh -- run the real boot chain FROM THE SPL ONWARD on QEMU xilinx-zynq-a9:
#   a test-only SPL (copy of the U-Boot tree + two QEMU patches) loads the PRODUCTION u-boot.img,
#   which runs the production boot.scr, pl.bit, zImage, DTB and rootfs -- all read from the
#   production SD image. This is the stage qemu_test.sh skips (it hands QEMU the kernel directly),
#   which is how an SPL with no console/MMC node reached the card on 2026-09-24.
#   The two patches, and why: QEMU 6.2 has no DDR controller or PLL model, so ps7_init() would poll
#   forever -> skipped; QEMU's SLCR reports boot mode JTAG -> spl_boot_device() forced to SD.
#   NOT covered: the BootROM loading boot.bin, ps7_init (clocks, DDR training) -- hardware only.
#     wsl -d Ubuntu-22.04 -u root --exec bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_spl_chain_test.sh
set -uo pipefail
SRC=/root/zynq/u-boot; T=/root/zynq/u-boot-qemutest; IMG=/root/zynq/pz7020-starlite-sd.img
LOG=/root/zynq/qemu-spl-chain.log; TIMEOUT=${TIMEOUT:-600}
rm -rf "$T"; mkdir -p "$T"; (cd "$SRC" && tar --exclude=.git -cf - .) | (cd "$T" && tar -xf -); cd "$T"
python3 - <<'PY'
p="arch/arm/mach-zynq/spl.c"; s=open(p).read()
a="#if !defined(CONFIG_DEBUG_UART_BOARD_INIT)\n\tps7_init();\n#endif\n"   # the call board_init_f makes (the other one is debug-UART only)
assert s.count(a)==1; s=s.replace(a,"\t/* QEMU TEST BUILD: no DDRC/PLL model in QEMU, ps7_init() skipped */\n")
a="u32 spl_boot_device(void)\n{\n\tu32 mode;\n"
assert s.count(a)==1; s=s.replace(a, a+"\n\treturn BOOT_DEVICE_MMC1;\t/* QEMU TEST BUILD: QEMU's SLCR reports JTAG */\n")
open(p,"w").write(s); print("test patches applied to", p)
PY
[ $? -eq 0 ] || { echo "SPL CHAIN: test patches did not apply -- NOT running an unpatched SPL"; exit 1; }
export CROSS_COMPILE=arm-linux-gnueabihf- ARCH=arm
make -j"$(nproc)" DEVICE_TREE=zynq-pz7020-starlite 2>&1 | grep -E "error|Error" ; ls -la spl/u-boot-spl
cp "$IMG" /root/zynq/qemu-chain-sd.img && truncate -s 2G /root/zynq/qemu-chain-sd.img
echo "production image: $(sha256sum "$IMG" | cut -c1-16)...  QEMU start $(date +%T)"
timeout "$TIMEOUT" qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -serial mon:stdio \
  -kernel spl/u-boot-spl -drive file=/root/zynq/qemu-chain-sd.img,if=sd,format=raw </dev/null >"$LOG" 2>&1
echo "QEMU end $(date +%T)"
grep -a -n -E "U-Boot SPL|Trying to boot|uImage|^U-Boot 20|DRAM:|^MMC:|Found U-Boot script|Loading PL|Starting kernel|Booting Linux|EXT4-fs \(mmcblk0p2\): mounted|automatic login|ZYNQ-REPORT|Kernel panic|### ERROR|resetting|FAIL" "$LOG" | cut -c1-160 | head -60
grep -a -q "ZYNQ-REPORT END" "$LOG" && echo "SPL CHAIN: SPL -> U-Boot -> boot.scr -> Linux -> autologin -> report, all reached" || { echo "SPL CHAIN: did not reach the report"; tail -25 "$LOG" | cut -c1-160; }
