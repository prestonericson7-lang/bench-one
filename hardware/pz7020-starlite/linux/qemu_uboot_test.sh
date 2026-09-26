#!/bin/bash
# qemu_uboot_test.sh -- run the PRODUCTION U-Boot proper (u-boot-dtb.bin, the same code and device tree as
# u-boot.img) on QEMU xilinx-zynq-a9 at 512 MB with the production SD image: U-Boot finds boot.scr, the
# script loads pl.bit into memory U-Boot has not reserved, runs fpga loadb, loads zImage + DTB and starts
# Linux. This is the stage where a reserved-memory window at the pl.bit staging address stops the boot
# ("Reading file would overwrite reserved memory") -- on the board that means DONE never lights.
# NOT covered: the BootROM, the SPL and ps7_init (hardware only; see BOOT-SD-runbook.md).
#     wsl -d Ubuntu-22.04 -u root --exec bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_uboot_test.sh
set -uo pipefail
UB=${UB:-/root/zynq/u-boot/u-boot-dtb.bin}
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
T=${TIMEOUT:-240}
W=$(mktemp -d /tmp/quboot.XXXX); trap 'rm -rf "$W"' EXIT
cp "$IMG" "$W/sd.img" && truncate -s 2G "$W/sd.img"
echo "u-boot-dtb.bin $(sha256sum < "$UB" | cut -c1-16)  image $(sha256sum < "$IMG" | cut -c1-16)"
timeout "$T" qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -serial mon:stdio \
  -device loader,file="$UB",addr=0x04000000,force-raw=on -device loader,addr=0x04000000,cpu-num=0 \
  -drive file="$W/sd.img",if=sd,format=raw </dev/null > "$W/log" 2>&1
tr -d '\r' < "$W/log" > "$W/l"
grep -a -E "^U-Boot 20|^DRAM:|^MMC:|Found U-Boot script|Loading PL bitstream|fpga loadb|would overwrite reserved|No pl.bit|Starting kernel|Booting Linux|Kernel command line|ERROR|error" "$W/l" | cut -c1-160 | head -30
fail=0
chk() { if grep -aq "$2" "$W/l"; then echo "  PASS  $1"; else echo "  FAIL  $1"; fail=1; fi; }
chk "U-Boot proper runs, DRAM from its DT is 512 MiB" "^DRAM:.*512 MiB"
chk "boot.scr found and run" "Found U-Boot script\|## Executing script"
chk "pl.bit loaded (not refused as reserved memory)" "Loading PL bitstream pl.bit"
if grep -aq "would overwrite reserved memory" "$W/l"; then echo "  FAIL  U-Boot refused a load into reserved memory"; fail=1; fi
chk "kernel started with the PL marker decision made" "Starting kernel"
chk "Linux booting" "Booting Linux"
[ $fail = 0 ] && echo "U-BOOT STAGE: PASS" || { echo "U-BOOT STAGE: FAIL"; tail -20 "$W/l" | cut -c1-160; }
exit $fail
