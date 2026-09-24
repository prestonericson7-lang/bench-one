#!/bin/bash
# build_uboot.sh -- U-Boot with SPL for the PZ7020-StarLite, no Vivado, no FSBL.
#   SPL runs the board's hardware-validated ps7_init_gpl.c (clocks, MIO, DDR training), then loads
#   u-boot.img from the FAT partition. Run INSIDE WSL Ubuntu-22.04 as root:
#     wsl -d Ubuntu-22.04 -u root -- bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/build_uboot.sh
# Outputs (copied to $OUT): boot.bin (BootROM image = SPL), u-boot.img, boot.scr, the .dtb
set -euo pipefail
REPO=${REPO:-/mnt/d/espicpc}
SRC=${SRC:-/root/zynq/u-boot}
OUT=${OUT:-$REPO/hardware/pz7020-starlite/linux/out}
BOARD=pz7020-starlite
DT=zynq-$BOARD
export CROSS_COMPILE=arm-linux-gnueabihf- ARCH=arm
J=$(nproc)
mkdir -p "$OUT"
cd "$SRC"

# --- board files: ps7_init from the validated XSA, device tree from the repo ---
# v2025.07: board/xilinx/zynq/Makefile sets hw-platform-y := $(DEVICE_TREE) -> the dir must be named exactly like the DT
mkdir -p board/xilinx/zynq/$DT
cp "$REPO/hardware/pz7020-starlite/ps7/ps7_init_gpl.c" "$REPO/hardware/pz7020-starlite/ps7/ps7_init_gpl.h" board/xilinx/zynq/$DT/
# Device tree placement is decided AFTER the defconfig (OF_UPSTREAM / vendor dir), see below.
# --- how the Zynq board Makefile finds ps7_init_gpl.c: by the hw-platform name derived from the DT ---
echo "board/xilinx/zynq/Makefile ps7_init rule:"; grep -n -E "ps7_init|hw-platform|hw_platform" board/xilinx/zynq/Makefile | head -8

make -s mrproper
make -s xilinx_zynq_virt_defconfig
OFU=$(grep -E '^CONFIG_OF_UPSTREAM=y' .config || true); VENDOR=$(sed -n 's/^CONFIG_OF_UPSTREAM_VENDOR="\(.*\)"//p' .config)
echo "OF_UPSTREAM='$OFU' vendor='$VENDOR'"; sed -n '13,24p' dts/Makefile
if [ -n "$OFU" ]; then DTDIR="dts/upstream/src/arm${VENDOR:+/$VENDOR}"; else DTDIR="arch/arm/dts"; fi
mkdir -p "$DTDIR"; cp "$REPO/hardware/pz7020-starlite/linux/$DT.dts" "$DTDIR/"
cp "$REPO/hardware/pz7020-starlite/linux/$DT.dts" arch/arm/dts/
grep -q "$DT.dtb" arch/arm/dts/Makefile || printf 'dtb-$(CONFIG_ARCH_ZYNQ) += %s.dtb
' "$DT" >> arch/arm/dts/Makefile
echo "DT placed in $DTDIR"
scripts/config --set-str DEFAULT_DEVICE_TREE "$DT"
scripts/config --set-str SPL_FS_LOAD_PAYLOAD_NAME "u-boot.img"
scripts/config --enable  CMD_FPGA
scripts/config --enable  FPGA_XILINX
scripts/config --enable  FPGA_ZYNQPL
# bootcmd: keep U-Boot's default distro boot -- it scans mmc 0:1 for boot.scr and sources it
make -s olddefconfig
make -j"$J" DEVICE_TREE="$DT" 2>&1 | grep -E "error|Error|warning: .*ps7|ps7_init" || true

ls -la spl/boot.bin u-boot.img u-boot.dtb
# the SPL must carry OUR ps7_init: look for a DDR-controller register write unique to it
arm-linux-gnueabihf-objdump -d spl/u-boot-spl | grep -c "ps7_init" || true
nm spl/u-boot-spl | grep -E " ps7_init| ps7_post_config" || { echo "*** ps7_init symbols missing from SPL"; exit 1; }

mkimage -A arm -T script -C none -d "$REPO/hardware/pz7020-starlite/linux/boot.cmd" boot.scr >/dev/null
cp spl/boot.bin u-boot.img boot.scr "$OUT/"
cp "$DTDIR/$DT.dtb" "$OUT/" 2>/dev/null || cp arch/arm/dts/$DT.dtb "$OUT/" 2>/dev/null || cp u-boot.dtb "$OUT/$DT.dtb"
sha256sum "$OUT"/boot.bin "$OUT"/u-boot.img "$OUT"/boot.scr | tee "$OUT/uboot.sha256"
echo "U-BOOT DONE: $OUT"
