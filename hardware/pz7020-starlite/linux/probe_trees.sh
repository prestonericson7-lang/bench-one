#!/bin/bash
# probe_trees.sh -- facts about the WSL U-Boot/Linux trees needed for the FSBL flow
K=/root/zynq/linux/.config; U=/root/zynq/u-boot/.config
echo "--- kernel config ---"
grep -E "^CONFIG_(XILINX_GMII2RGMII|MACB|REALTEK_PHY|UIO|UIO_PDRV_GENIRQ|FPGA_MGR_ZYNQ_FPGA|OF_OVERLAY|FPGA_REGION|OF_FPGA_REGION|XILINX_EMACLITE)=" $K
grep -E "XILINX_GMII2RGMII" $K
echo "--- u-boot config ---"
grep -E "^CONFIG_(REMAKE_ELF|OF_SEPARATE|OF_EMBED|DEFAULT_DEVICE_TREE|TEXT_BASE|SYS_TEXT_BASE|SPL|BOOTDELAY|DISTRO_DEFAULTS|BOOTSTD|FPGA_ZYNQPL|CMD_FPGA)=" $U
ls -la /root/zynq/u-boot/u-boot.elf /root/zynq/u-boot/u-boot.bin /root/zynq/u-boot/u-boot-dtb.bin 2>&1 | cut -c1-120
echo "--- modules on the card's rootfs image ---"
ls /root/zynq/rootfs/lib/modules/*/kernel/drivers/net/phy/ 2>/dev/null | head -20
