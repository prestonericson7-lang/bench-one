#!/bin/bash
# rebuild_linux_dtb.sh -- recompile only the Linux device tree from the repo DTS and put it in out/
set -euo pipefail
REPO=/mnt/d/espicpc; OUT=${OUT:-$REPO/hardware/pz7020-starlite/linux/out}; DT=zynq-pz7020-starlite
DTS_SRC=${DTS_SRC:-$REPO/hardware/pz7020-starlite/linux/$DT.dts}
cd /root/zynq/linux
OLD=/root/zynq/linux-old.dtb; cp arch/arm/boot/dts/xilinx/$DT.dtb "$OLD"
cp "$DTS_SRC" arch/arm/boot/dts/xilinx/$DT.dts
make -s ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- xilinx/$DT.dtb
cp arch/arm/boot/dts/xilinx/$DT.dtb "$OUT/"
echo "--- decompiled diff, old kernel DTB -> new (only bootph-all lines expected):"
diff <(dtc -I dtb -O dts "$OLD" 2>/dev/null) <(dtc -I dtb -O dts arch/arm/boot/dts/xilinx/$DT.dtb 2>/dev/null) || true
ls -la "$OUT/$DT.dtb"; sha256sum "$OUT/$DT.dtb"
