#!/bin/bash
L=/root/zynq/linux
f=$(ls $L/Documentation/devicetree/bindings/net/*gmii*rgmii* 2>/dev/null | head -1); echo "binding: $f"; sed -n '1,80p' "$f"
echo "--- driver Kconfig ---"; grep -n -B2 -A8 "config XILINX_GMII2RGMII" $L/drivers/net/phy/Kconfig
echo "--- MODVERSIONS ---"; grep -E "^CONFIG_MODVERSIONS|^CONFIG_MODULE_SIG=" $L/.config
