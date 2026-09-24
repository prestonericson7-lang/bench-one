#!/bin/bash
# rebuild_boot.sh -- regenerate boot.scr from boot.cmd, reassemble the SD image, re-run the QEMU dry run.
set -e
REPO=/mnt/d/espicpc; OUT=$REPO/hardware/pz7020-starlite/linux/out
mkimage -A arm -T script -C none -d "$REPO/hardware/pz7020-starlite/linux/boot.cmd" "$OUT/boot.scr" >/dev/null
bash "$REPO/hardware/pz7020-starlite/linux/mk_sd_image.sh" 2>&1 | grep -E "sha256|SD IMAGE|pl.bit"
TIMEOUT=200 bash "$REPO/hardware/pz7020-starlite/linux/qemu_test.sh" 2>&1 | grep -E "QEMU BOOT|login:|eth0|end0|zynq-agent" | cut -c1-140
