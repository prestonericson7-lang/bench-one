#!/bin/bash
# build_fallback_512m.sh -- the 16-bit / 512 MB boot set, kept ready in out/fallback-512MB/.
# This is the configuration that already booted this board to the kernel on 2026-09-24 (third-party
# ps7_init, one DRAM chip in use). Use it only if the 32-bit / 1 GB set fails at DDR init.
set -euo pipefail
REPO=/mnt/d/espicpc; L=$REPO/hardware/pz7020-starlite/linux; DT=zynq-pz7020-starlite
F=$L/out/fallback-512MB; mkdir -p "$F"
sed -e 's|reg = <0x0 0x40000000>;.*|reg = <0x0 0x20000000>;\t/* 512 MB fallback: 16-bit DDR (ps7/ps7_init_gpl.c) */|' "$L/$DT.dts" > /tmp/$DT-512m.dts
# the 16-bit PS config does not clock GEM1 over EMIO or FCLK1 -> no PL Ethernet in this fallback
# and the matrix engine's 384 MB at 0x20000000 lies beyond 512 MB: drop its window, so zaccel-server
# runs its CPU engine. The GPU's window (0x1E000000, 32 MB) is inside 512 MB and stays.
printf '
&gem1 {
	status = "disabled";
};
/ {
	reserved-memory {
		/delete-node/ zaccel@20000000;
	};
};
&amba {
	/delete-node/ zaccel-mem@20000000;
	/delete-node/ zaccel-dma@40400000;
};
' >> /tmp/$DT-512m.dts
grep -n -E "0x20000000|disabled" /tmp/$DT-512m.dts | tail -3
OUT=$F DTS_SRC=/tmp/$DT-512m.dts PS7_DIR=$REPO/hardware/pz7020-starlite/ps7 bash "$L/build_uboot.sh" | grep -E "ps7_init source|SPL DTB|F8006000|U-BOOT DONE"
OUT=$F DTS_SRC=/tmp/$DT-512m.dts bash "$L/rebuild_linux_dtb.sh" | grep -E "^[<>].*reg = |sha256|dtb$" | head -4
# restore the primary (1 GB) DTS into the U-Boot and kernel trees so later builds start from it
cp "$L/$DT.dts" /root/zynq/u-boot/arch/arm/dts/$DT.dts; cp "$L/$DT.dts" /root/zynq/linux/arch/arm/boot/dts/xilinx/$DT.dts
ls -la "$F"
