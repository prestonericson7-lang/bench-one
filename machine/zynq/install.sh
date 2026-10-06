#!/bin/bash
# install.sh -- put the machine's pieces into a Zynq rootfs tree (called by mk_sd_image.sh with the
# rootfs directory, or by hand on a running board with ROOT=/). Nothing here touches the first stage:
# boot.bin, u-boot.img, zImage, the DTB and pl.bit are the tested set and stay as they are.
#   bash install.sh /root/zynq/rootfs
set -euo pipefail
ROOT=${1:-/}
H=$(cd "$(dirname "$0")" && pwd)
install -D -m 0755 "$H/zynq-node"          "$ROOT/usr/local/bin/zynq-node"
install -D -m 0644 "$H/zynq-node.service"  "$ROOT/etc/systemd/system/zynq-node.service"
mkdir -p "$ROOT/etc/systemd/system/sysinit.target.wants"
ln -sf /etc/systemd/system/zynq-node.service "$ROOT/etc/systemd/system/sysinit.target.wants/zynq-node.service"
# the earlier attempt as a networkd drop-in ran inside networkd's sandbox and changed nothing: remove it
rm -f "$ROOT/etc/systemd/system/systemd-networkd.service.d/zynq-node.conf"
rmdir "$ROOT/etc/systemd/system/systemd-networkd.service.d" 2>/dev/null || true
# The Orange Pi's swap on this board ("zynqram", accel/mem, enabled by install_zynq.sh earlier in the
# same image build) is KEPT: card #1 goes back next to the Pi, whose zaccel-swap uses it (2026-10-05).
# A version of this script on 2026-09-30 masked nbd-server (a symlink to /dev/null; Debian starts
# nbd-server from its SysV script, so that mask is the only thing that stops it) and deleted the
# zynqram-prep links -- undo the mask in a build rootfs that still carries it.
if [ -L "$ROOT/etc/systemd/system/nbd-server.service" ] && [ "$(readlink "$ROOT/etc/systemd/system/nbd-server.service")" = /dev/null ]; then
  rm -f "$ROOT/etc/systemd/system/nbd-server.service" && echo "machine: nbd-server unmasked (the Pi's swap export)"
fi
install -D -m 0755 "$H/machine-bench"      "$ROOT/usr/local/bin/machine-bench"
mkdir -p "$ROOT/usr/local/lib/machine" "$ROOT/opt/machine/models" "$ROOT/boot/reports"
for b in run_model ppl tl_ref zaccel-bench test_lib; do
  if [ -f "$H/out/armhf/$b" ]; then install -m 0755 "$H/out/armhf/$b" "$ROOT/usr/local/lib/machine/$b"; else echo "note: $b not built (machine/zynq/build.sh)"; fi
done
sed -i 's/\r$//' "$ROOT/usr/local/bin/zynq-node" "$ROOT/usr/local/bin/machine-bench" "$ROOT/etc/systemd/system/zynq-node.service"
# a model placed in out/models/ is carried in the image (the download is the owner's call)
for m in "$H"/out/models/*.gguf; do [ -f "$m" ] && install -m 0644 "$m" "$ROOT/opt/machine/models/" && echo "model: $(basename "$m")"; done
echo "machine: installed into $ROOT"
