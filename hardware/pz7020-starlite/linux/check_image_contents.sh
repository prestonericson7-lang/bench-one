#!/bin/bash
# check_image_contents.sh -- prove the SD image carries what the repo says it does: the same agent
# code, the same boot files, the networkd config and the agent unit. Read-only loop mounts of the
# image file; no card is touched. Run inside WSL as root.
#   bash check_image_contents.sh [image]      (default /root/zynq/pz7020-starlite-sd.img)
set -uo pipefail
REPO=/mnt/d/espicpc
L=$REPO/hardware/pz7020-starlite/linux
IMG=${1:-/root/zynq/pz7020-starlite-sd.img}
DT=zynq-pz7020-starlite
M=$(mktemp -d); rc=0
start() { sfdisk -J "$IMG" | python3 -c "import json,sys; print(json.load(sys.stdin)['partitiontable']['partitions'][$1]['start'] * 512)"; }
same() { if cmp -s "$1" "$2"; then echo "  same     $3"; else echo "  DIFFERS  $3"; rc=1; fi; }

mount -o ro,loop,offset="$(start 0)" "$IMG" "$M" || { echo "cannot mount the boot partition"; exit 2; }
for f in boot.bin u-boot.img boot.scr zImage $DT.dtb; do same "$M/$f" "$L/out/$f" "BOOT/$f = linux/out/$f"; done
same "$M/pl.bit" "$REPO/hardware/pz7020-starlite/vivado/build/system.bit" "BOOT/pl.bit = vivado/build/system.bit"
umount "$M"

mount -o ro,loop,offset="$(start 1)" "$IMG" "$M" || { echo "cannot mount the root partition"; exit 2; }
for f in zynq_agent.py pl_regs.py; do same "$M/usr/local/bin/$f" "$REPO/firmware/telemetry-hub/$f" "/usr/local/bin/$f = telemetry-hub/$f"; done
for f in etc/systemd/network/20-eth0.network etc/systemd/system/zynq-agent.service \
         etc/systemd/system/multi-user.target.wants/zynq-agent.service usr/local/bin/zynq-report; do
  # enable links are absolute symlinks: -e would resolve them against THIS machine's /etc
  if [ -L "$M/$f" ]; then echo "  present  /$f -> $(readlink "$M/$f")"
  elif [ -e "$M/$f" ]; then echo "  present  /$f"; else echo "  MISSING  /$f"; rc=1; fi
done
grep -q '^Address=10.20.0.2/24' "$M/etc/systemd/network/20-eth0.network" && echo "  eth0 static fallback 10.20.0.2/24 configured" \
  || { echo "  eth0 static fallback MISSING"; rc=1; }
umount "$M"; rmdir "$M"
echo "  image sha256 $(sha256sum "$IMG" | cut -c1-64)"
[ $rc = 0 ] && echo "IMAGE CONTENTS: PASS" || echo "IMAGE CONTENTS: FAIL"
exit $rc
