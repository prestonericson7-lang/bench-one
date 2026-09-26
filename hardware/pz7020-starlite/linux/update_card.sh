#!/bin/bash
# update_card.sh -- refresh the BOOT (FAT) partition of the PZ7020 SD card that is mounted in this PC.
# Only the boot files change (SPL, U-Boot, boot script, kernel, DTB, PL bitstream); the ext4 rootfs is
# left alone. Refuses to touch a drive that does not already look like this card's boot partition.
#   bash update_card.sh E                        the 32-bit / 1 GB set in linux/out
#   bash update_card.sh E out/fallback-512MB     the 16-bit / 512 MB set (the one that boots this board)
# pl.bit always comes from vivado/build/system.bit, the bitstream mk_sd_image.sh puts in the image (a
# copy in out/ went stale once and would have put the pre-accelerator bitstream on the card).
set -euo pipefail
DRV=${1:-E}
SET=${2:-out}
CARD=/$(echo "$DRV" | tr 'A-Z' 'a-z')
OUT=$(cd "$(dirname "$0")/$SET" && pwd)
FILES="boot.bin u-boot.img boot.scr zImage zynq-pz7020-starlite.dtb pl.bit"
PLBIT=$(cd "$(dirname "$0")/../vivado/build" && pwd)/system.bit
[ -f "$PLBIT" ] || { echo "missing $PLBIT"; exit 3; }

[ -d "$CARD" ] || { echo "no drive $DRV:"; exit 1; }
for f in boot.bin boot.scr zImage zynq-pz7020-starlite.dtb; do
  [ -f "$CARD/$f" ] || { echo "$DRV: has no $f -- not the PZ7020 boot partition, refusing"; exit 2; }
done
for f in $FILES; do [ "$f" = pl.bit ] || [ -f "$OUT/$f" ] || { echo "missing build output $OUT/$f"; exit 3; }; done
src() { [ "$1" = pl.bit ] && echo "$PLBIT" || echo "$OUT/$1"; }

echo "card $DRV: before"; ls -la "$CARD" | grep -v -E "^total|System Volume" | tail -n +3
for f in $FILES; do cp -f "$(src $f)" "$CARD/$f"; done
powershell.exe -NoProfile -Command "Write-VolumeCache -DriveLetter $DRV" && echo "volume cache flushed"
bad=0
for f in $FILES; do
  a=$(sha256sum "$(src $f)" | cut -d' ' -f1); b=$(sha256sum "$CARD/$f" | cut -d' ' -f1)
  if [ "$a" = "$b" ]; then echo "  ok  $f  $a"; else echo "  BAD $f  out=$a card=$b"; bad=1; fi
done
echo "card $DRV: after"; ls -la "$CARD" | grep -v -E "^total|System Volume" | tail -n +3
[ $bad -eq 0 ] && echo "CARD UPDATED" || { echo "CARD UPDATE FAILED"; exit 4; }
