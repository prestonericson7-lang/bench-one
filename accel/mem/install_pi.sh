#!/bin/bash
# install_pi.sh -- install zaccel-swap on the Orange Pi (run as root). Idempotent.
#   nbd-client from apt, the nbd module now and at every boot, the script and its units, enabled and
#   started. With no Zynq on the network it installs cleanly and the retry timer attaches later.
#   The official Orange Pi kernel (6.6.98-sun60iw2) is built without CONFIG_BLK_DEV_NBD. When
#   "modprobe nbd" fails, the module prebuilt for exactly this kernel by build_pi_nbd.sh
#   (pi-kmod/<uname -r>/nbd.ko, checked against its .sha256) goes into /lib/modules/<uname -r>/updates.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ "$(id -u)" = 0 ] || { echo "install_pi.sh: run as root" >&2; exit 1; }

if ! command -v nbd-client >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get install -y nbd-client || { apt-get update && apt-get install -y nbd-client; }
fi

KVER=$(uname -r)
KMOD=$HERE/pi-kmod/$KVER/nbd.ko
if ! modprobe nbd 2>/dev/null; then
  if [ ! -f "$KMOD" ]; then
    echo "install_pi.sh: kernel $KVER has no nbd module, and no prebuilt nbd module matches it ($KMOD does not exist); zaccel-swap cannot work" >&2
    exit 1
  fi
  # build_pi_nbd.sh writes plain sha256sum output (hash, then its own build path): compare the hash only
  want=$(awk '{ print $1; exit }' "$KMOD.sha256" 2>/dev/null || true)
  got=$(sha256sum "$KMOD" | awk '{ print $1 }')
  if [ -z "$want" ] || [ "$want" != "$got" ]; then
    echo "install_pi.sh: $KMOD does not match $KMOD.sha256 (expected '${want:-no .sha256 file}', file is $got); not installing it" >&2
    exit 1
  fi
  install -D -m 0644 "$KMOD" "/lib/modules/$KVER/updates/nbd.ko"
  depmod -a
  modprobe nbd || { echo "install_pi.sh: modprobe nbd still fails after installing $KMOD as /lib/modules/$KVER/updates/nbd.ko" >&2; exit 1; }
  echo "install_pi.sh: installed the prebuilt nbd module for $KVER as /lib/modules/$KVER/updates/nbd.ko (sha256 $got)"
fi
echo nbd > /etc/modules-load.d/nbd.conf

install -D -m 0755 "$HERE/zaccel-swap" /usr/local/sbin/zaccel-swap
install -m 0644 "$HERE/zaccel-swap.service" "$HERE/zaccel-swap-retry.service" "$HERE/zaccel-swap-retry.timer" \
  /etc/systemd/system/
systemctl daemon-reload
systemctl enable zaccel-swap.service
systemctl start zaccel-swap.service
systemctl --no-pager --lines=0 status zaccel-swap.service || true
echo "install_pi.sh: done; 'swapon --show' lists the Zynq swap once it is attached"
