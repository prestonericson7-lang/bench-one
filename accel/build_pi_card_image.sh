#!/bin/bash
# build_pi_card_image.sh -- the Orange Pi's card image: the OFFICIAL Orange Pi 4 Pro 1.1.0 Ubuntu image
# (hash checked against the vendor's .sha), with the accelerator bundle staged into its rootfs
# (stage_pi_card.sh root). At its first boot the Pi installs everything by itself: offline packages,
# NVMe prepared and mounted at /mnt/nvme, Zynq swap, GPU tools, Teensy firmware + flasher.
# Why a whole image: the card cannot be edited in place from this PC (wsl --mount does not take USB
# card readers), and the owner's Pi card holds nothing to keep. WSL, root. Output (git-ignored):
#   $OUTDIR/opi4pro-accel.img + .sha256   -> write with hardware/pz7020-starlite/linux/write_sd.py
set -euo pipefail
SRC=${OPI_IMG:-/mnt/c/Users/Danie/Downloads/opi4pro/Orangepi4pro_1.1.0_ubuntu_resolute_desktop_xfce_linux6.6.98.img}
A=$(cd "$(dirname "$0")" && pwd)
OUTDIR=${OUTDIR:-$A/pi-card}                     # git-ignored (accel/.gitignore)
W=/root/opi; IMG=$W/opi4pro-accel.img
mkdir -p "$W" "$OUTDIR"

echo "== the official image, checked against the vendor's hash"
want=$(awk '{ print $1 }' "$SRC.sha")
got=$(sha256sum "$SRC" | awk '{ print $1 }')
echo "vendor .sha $want"; echo "image       $got"
[ "$want" = "$got" ] || { echo "the stock image does not match the vendor hash -- stop"; exit 1; }

echo "== copy, then stage the bundle into its rootfs"
cp --sparse=always "$SRC" "$IMG"
# MODEL=<file.gguf> [MODEL_NAME=<name>.gguf]: pre-load a model for bench-day. The stock rootfs has ~1.7 GB
# free, so the image and its one partition grow by the model's size + 512 MiB first (the Pi's own
# first-boot resize then grows it to the whole card as usual).
MODEL=${MODEL:-}
MODEL_NAME=${MODEL_NAME:-$(basename "${MODEL:-x.gguf}")}
if [ -n "$MODEL" ]; then
  [ -f "$MODEL" ] || { echo "no model at $MODEL"; exit 1; }
  grow=$(( $(stat -c %s "$MODEL") / 1048576 + 512 ))
  truncate -s +${grow}M "$IMG"
  echo ", +" | sfdisk --quiet --no-reread -N 1 "$IMG"
  L=$(losetup -f --show -o $((32 * 1024 * 1024)) "$IMG")
  e2fsck -fy "$L" >/dev/null 2>&1; resize2fs "$L" >/dev/null 2>&1 || { losetup -d "$L"; echo "resize2fs failed"; exit 1; }
  losetup -d "$L"
  echo "image grown by $grow MiB for the model"
fi
M=$(mktemp -d); L=$(losetup -f --show -o $((32 * 1024 * 1024)) "$IMG")
trap 'mountpoint -q "$M" && umount "$M"; losetup -d "$L" 2>/dev/null; rmdir "$M" 2>/dev/null' EXIT
mount -t ext4 "$L" "$M"
bash "$A/stage_pi_card.sh" root "$M"
if [ -n "$MODEL" ]; then                   # the model bench-day runs by default
  install -D -m 0644 "$MODEL" "$M/opt/accel/models/$MODEL_NAME"
  sync; echo 3 > /proc/sys/vm/drop_caches
  cmp "$MODEL" "$M/opt/accel/models/$MODEL_NAME" && echo "model: /opt/accel/models/$MODEL_NAME read back identical" \
    || { echo "model copy does not read back identical -- stop"; exit 1; }
fi
sync; umount "$M"
e2fsck -fn "$L" > "$W/fsck.log" 2>&1 && echo "e2fsck -fn: clean" || { cat "$W/fsck.log"; echo "filesystem not clean -- stop"; exit 1; }
losetup -d "$L"; trap - EXIT; rmdir "$M"

echo "== output"
sha=$(sha256sum "$IMG" | awk '{ print $1 }')
echo "$sha  opi4pro-accel.img" > "$W/opi4pro-accel.img.sha256"
cp --sparse=always "$IMG" "$OUTDIR/opi4pro-accel.img"
cp "$W/opi4pro-accel.img.sha256" "$OUTDIR/"
[ "$(sha256sum "$OUTDIR/opi4pro-accel.img" | awk '{ print $1 }')" = "$sha" ] || { echo "copy to $OUTDIR does not match"; exit 1; }
ls -la "$OUTDIR"
echo "PI CARD IMAGE DONE: $OUTDIR/opi4pro-accel.img sha256 $sha"
