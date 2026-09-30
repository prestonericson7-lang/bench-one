#!/bin/bash
# install.sh -- put the machine's pieces into a Zynq rootfs tree (called by mk_sd_image.sh with the
# rootfs directory, or by hand on a running board with ROOT=/). Nothing here touches the first stage:
# boot.bin, u-boot.img, zImage, the DTB and pl.bit are the tested set and stay as they are.
#   bash install.sh /root/zynq/rootfs
set -euo pipefail
ROOT=${1:-/}
H=$(cd "$(dirname "$0")" && pwd)
install -D -m 0755 "$H/zynq-node"          "$ROOT/usr/local/bin/zynq-node"
install -D -m 0644 "$H/zynq-node.conf"     "$ROOT/etc/systemd/system/systemd-networkd.service.d/zynq-node.conf"
install -D -m 0755 "$H/machine-bench"      "$ROOT/usr/local/bin/machine-bench"
mkdir -p "$ROOT/usr/local/lib/machine" "$ROOT/opt/machine/models" "$ROOT/boot/reports"
for b in run_model ppl tl_ref zaccel-bench test_lib; do
  if [ -f "$H/out/armhf/$b" ]; then install -m 0755 "$H/out/armhf/$b" "$ROOT/usr/local/lib/machine/$b"; else echo "note: $b not built (machine/zynq/build.sh)"; fi
done
sed -i 's/\r$//' "$ROOT/usr/local/bin/zynq-node" "$ROOT/usr/local/bin/machine-bench" "$ROOT/etc/systemd/system/systemd-networkd.service.d/zynq-node.conf"
# a model placed beside this script's out/ dir is carried in the image (the download is the owner's call)
for m in "$H"/out/models/*.gguf; do [ -f "$m" ] && install -m 0644 "$m" "$ROOT/opt/machine/models/" && echo "model: $(basename "$m")"; done
echo "machine: installed into $ROOT"
