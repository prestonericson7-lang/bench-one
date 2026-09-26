#!/bin/bash
# check_uboot_mem.sh -- the memory node U-Boot proper will report (DRAM: ...) for each boot set
O=/mnt/d/espicpc/hardware/pz7020-starlite/linux/out
for f in "$O/u-boot-zynq-pz7020-starlite.dtb"; do
  echo "== ${f#$O/}"; dtc -q -I dtb -O dts "$f" | grep -A3 "memory@0" | grep reg
done
