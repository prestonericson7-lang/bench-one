#!/bin/bash
# install_zynq.sh ROOTFS -- drop the Zynq side of zaccel-swap (the "zynqram" nbd export) into a
# Debian bookworm armhf root filesystem. Runs nothing from the target, so it works on a foreign-arch
# rootfs from the image builder. The packages in zynq-packages.txt must be installed in ROOTFS too
# (before or after this; nothing here depends on the order).
#   /etc/default/zynqram                    ZACCEL_SWAP_MB=128 (an existing file is kept)
#   /etc/nbd-server/conf.d/zynqram.conf     export [zynqram], TCP 10809
#   /etc/nbd-server/zynqram.allow           who may attach: the Pi's links only
#   /usr/local/sbin/zynqram-prep            tmpfs + fully allocated file, at boot
#   /etc/systemd/system/zynqram-prep.service  enabled: RequiredBy nbd-server, WantedBy multi-user
set -euo pipefail
ROOT=${1:?usage: install_zynq.sh ROOTFS}
[ -d "$ROOT" ] || { echo "install_zynq.sh: no such directory: $ROOT" >&2; exit 1; }
ROOT=${ROOT%/}                     # "/" -> "", so "$ROOT/etc" is "/etc"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=$HERE/zynq

if [ -e "$ROOT/etc/default/zynqram" ]; then
  echo "install_zynq.sh: keeping existing $ROOT/etc/default/zynqram"
else
  install -D -m 0644 "$SRC/zynqram.default" "$ROOT/etc/default/zynqram"
fi
install -D -m 0644 "$SRC/zynqram.conf"         "$ROOT/etc/nbd-server/conf.d/zynqram.conf"
install -D -m 0644 "$SRC/zynqram.allow"        "$ROOT/etc/nbd-server/zynqram.allow"
# Listen on IPv4 only. With nbd-server's default ("::, 0.0.0.0") an IPv4 client arrives on the IPv6
# socket as ::ffff:a.b.c.d, and address_matches() (nbdsrv.c, 3.23 and 3.24 alike) then compares only
# the first <masklen> bits of that 16-byte address against a CIDR line -- all zeros for every client --
# so "10.20.0.0/24" would admit anyone. On an IPv4 socket the comparison is the correct same-family one.
CFG=$ROOT/etc/nbd-server/config
if [ -f "$CFG" ] && ! grep -q '^[[:space:]]*listenaddr' "$CFG"; then
  sed -i '/^\[generic\]/a\	listenaddr = 0.0.0.0' "$CFG"
  echo "install_zynq.sh: nbd-server listens on IPv4 only (listenaddr = 0.0.0.0 in $CFG)"
elif [ ! -f "$CFG" ]; then
  echo "install_zynq.sh: WARNING: no $CFG -- install nbd-server first, then rerun (the allow list needs listenaddr)" >&2
fi
install -D -m 0755 "$SRC/zynqram-prep"         "$ROOT/usr/local/sbin/zynqram-prep"
install -D -m 0644 "$SRC/zynqram-prep.service" "$ROOT/etc/systemd/system/zynqram-prep.service"

# what "systemctl enable zynqram-prep" does, from its [Install] section
for dir in nbd-server.service.requires multi-user.target.wants; do
  mkdir -p "$ROOT/etc/systemd/system/$dir"
  ln -sfn /etc/systemd/system/zynqram-prep.service "$ROOT/etc/systemd/system/$dir/zynqram-prep.service"
done

if [ ! -e "$ROOT/bin/nbd-server" ] && [ ! -e "$ROOT/usr/bin/nbd-server" ]; then
  echo "install_zynq.sh: note: nbd-server is not installed in $ROOT yet (zynq-packages.txt: $(tr '\n' ' ' < "$HERE/zynq-packages.txt"))"
fi
echo "install_zynq.sh: zynqram export installed into ${ROOT:-/}"
