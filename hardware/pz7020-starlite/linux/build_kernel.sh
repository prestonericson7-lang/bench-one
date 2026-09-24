#!/bin/bash
# build_kernel.sh -- mainline Linux for the PZ7020-StarLite (zImage + dtb + modules), inside WSL as root:
#     wsl -d Ubuntu-22.04 -u root -- bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/build_kernel.sh
set -euo pipefail
REPO=${REPO:-/mnt/d/espicpc}
SRC=${SRC:-/root/zynq/linux}
OUT=${OUT:-$REPO/hardware/pz7020-starlite/linux/out}
DT=zynq-pz7020-starlite
export CROSS_COMPILE=arm-linux-gnueabihf- ARCH=arm
J=$(nproc)
mkdir -p "$OUT"
cd "$SRC"
cp "$REPO/hardware/pz7020-starlite/linux/$DT.dts" arch/arm/boot/dts/xilinx/
grep -q "$DT.dtb" arch/arm/boot/dts/xilinx/Makefile || printf 'dtb-$(CONFIG_ARCH_ZYNQ) += %s.dtb
' "$DT" >> arch/arm/boot/dts/xilinx/Makefile
grep -n "$DT" arch/arm/boot/dts/xilinx/Makefile | head -2

# v6.12 has no xilinx_zynq_defconfig any more; Zynq lives in multi_v7 (drivers mostly as modules)
make -s multi_v7_defconfig
# what the car/bench roles need on top of the Zynq defconfig
scripts/config --enable FPGA --enable FPGA_MGR_ZYNQ_FPGA --enable FPGA_REGION --enable OF_FPGA_REGION \
               --enable UIO --enable UIO_PDRV_GENIRQ \
               --enable USB_STORAGE --enable USB_SERIAL --enable USB_SERIAL_CH341 --enable USB_SERIAL_CP210X --enable USB_SERIAL_FTDI_SIO \
               --enable USB_ACM --enable REALTEK_PHY --enable SPI_SPIDEV --enable I2C_CHARDEV --enable GPIO_SYSFS --enable GPIO_CDEV_V1 \
               --enable MTD_SPI_NOR --enable SPI_ZYNQ_QSPI --enable EXT4_FS --enable VFAT_FS --enable NLS_CODEPAGE_437 --enable NLS_ISO8859_1 \
               --enable NFS_FS --enable ROOT_NFS --enable CIFS --enable IKCONFIG --enable IKCONFIG_PROC
make -s olddefconfig
make -j"$J" zImage dtbs modules 2>&1 | grep -E "error|Error" || true
ls -la arch/arm/boot/zImage arch/arm/boot/dts/xilinx/$DT.dtb
rm -rf /root/zynq/modules && make -s INSTALL_MOD_PATH=/root/zynq/modules modules_install >/dev/null
cp arch/arm/boot/zImage "$OUT/"; cp arch/arm/boot/dts/xilinx/$DT.dtb "$OUT/"
(cd /root/zynq/modules && tar czf "$OUT/modules.tar.gz" lib)
make -s kernelrelease | tee "$OUT/kernelrelease.txt"
sha256sum "$OUT/zImage" "$OUT/$DT.dtb" | tee -a "$OUT/kernel.sha256"
echo "KERNEL DONE: $OUT"
