#!/bin/bash
# stage_pi_card.sh -- put the accelerator bundle into an Orange Pi rootfs, so the Pi installs everything
# by itself at its next boot (accel-firstboot.service -> install_pi.sh: offline packages, NVMe at
# /mnt/nvme, Zynq swap, GPU tools, Teensy firmware + flasher, the wired-port address).
#   bash stage_pi_card.sh root <dir>    a mounted Pi rootfs: build_pi_card_image.sh's copy of the
#                                       official image, or the test's overlay of it
# (The card itself cannot be mounted from this PC: wsl --mount does not take USB card readers, so the
# card gets a whole image -- build_pi_card_image.sh, then write_sd.py.)
# Refuses anything that is not the official Orange Pi 4 Pro 1.1.0 rootfs. Writes only under
# /opt/accel/bundle and /etc/systemd/system on that filesystem.
set -euo pipefail
ROOT_UUID=a81ee6f1-5c12-44f0-8e92-c28029cab13c     # the official image's rootfs (its /etc/fstab, orangepiEnv.txt)
A=$(cd "$(dirname "$0")" && pwd)
TAR=${ACCEL_BUNDLE_TAR:-$A/accel-pi-bundle.tar.gz}
die() { echo "stage_pi_card: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root"
[ -f "$TAR" ] || die "no bundle at $TAR (make_pi_bundle.sh)"

is_opi_root() {  # <dir>: the stock Orange Pi 4 Pro Ubuntu rootfs, by content
  grep -qx 'VERSION_CODENAME=resolute' "$1/etc/os-release" 2>/dev/null &&
  grep -qx 'fdtfile=allwinner/sun60i-a733-orangepi-4-pro.dtb' "$1/boot/orangepiEnv.txt" 2>/dev/null &&
  grep -qx "rootdev=UUID=$ROOT_UUID" "$1/boot/orangepiEnv.txt" 2>/dev/null
}

stage() {  # <root>
  local R=$1 B=$1/opt/accel/bundle
  is_opi_root "$R" || die "$R is not the Orange Pi 4 Pro 1.1.0 rootfs (os-release / orangepiEnv.txt) -- refusing"
  echo "rootfs: $R ($(df -h --output=size,avail "$R" | tail -1 | sed 's/  */ /g') free)"
  rm -rf "$B"; mkdir -p "$B"
  tar -xzf "$TAR" -C "$B"
  chmod +x "$B"/*.sh "$B"/pi/*.sh "$B"/pi/nvme-auto "$B"/mem/*.sh "$B"/mem/zaccel-swap \
           "$B"/pi/out/aarch64/* "$B"/llm/out/aarch64/* "$B"/gpu/pi/*.sh 2>/dev/null || true
  install -m 0644 "$B/pi/accel-firstboot.service" "$R/etc/systemd/system/accel-firstboot.service"
  mkdir -p "$R/etc/systemd/system/multi-user.target.wants"
  ln -sfn /etc/systemd/system/accel-firstboot.service "$R/etc/systemd/system/multi-user.target.wants/accel-firstboot.service"
  rm -f "$R/var/lib/accel/firstboot.done"                    # a fresh stage installs again
  { echo "staged $(date -Is)"; sha256sum "$TAR" | awk '{ print "bundle sha256 " $1 }'; } > "$B/STAGED"
  sync
}

verify() {  # <root>: every bundle file as read back from the filesystem equals the tarball's
  local R=$1 T; T=$(mktemp -d); tar -xzf "$TAR" -C "$T"
  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true     # read back from the card, not the cache
  local n=0 bad=0 f
  while IFS= read -r -d '' f; do
    n=$((n + 1)); cmp -s "$T/$f" "$R/opt/accel/bundle/$f" || { echo "  differs: $f"; bad=$((bad + 1)); }
  done < <(cd "$T" && find . -type f -print0)
  rm -rf "$T"
  [ -L "$R/etc/systemd/system/multi-user.target.wants/accel-firstboot.service" ] || { echo "  unit not enabled"; bad=$((bad + 1)); }
  cmp -s "$R/etc/systemd/system/accel-firstboot.service" "$R/opt/accel/bundle/pi/accel-firstboot.service" || { echo "  unit differs"; bad=$((bad + 1)); }
  echo "read back: $n files, $bad problems"
  [ $bad = 0 ]
}

case ${1:-} in
  root)
    [ -d "${2:-}" ] || die "usage: $0 root <mounted rootfs dir>"
    stage "$2"; verify "$2"; echo "STAGED: $2" ;;
  *) die "usage: $0 root <mounted rootfs dir>" ;;
esac
