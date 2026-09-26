#!/bin/bash
# check_dtb.sh -- decompile the Linux DTB in out/ and show the nodes this board depends on
D=/mnt/d/espicpc/hardware/pz7020-starlite/linux/out/zynq-pz7020-starlite.dtb
dtc -q -I dtb -O dts "$D" > /tmp/k.dts
grep -n -E "reg = <0x00 0x20000000>|zaccel|operating-points|ethernet@e000c000|gmii-to-rgmii|xlnx,gmii-to-rgmii|ethernet-phy@2|ethernet-phy@1|phy-mode|generic-uio|pl_regs@40000000|ethernet1" /tmp/k.dts
