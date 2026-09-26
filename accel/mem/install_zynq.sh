#!/bin/bash
# install_zynq.sh ROOTFS -- drop the Zynq side of zaccel-swap (the "zynqram" nbd export) into a
# Debian bookworm armhf root filesystem. Runs nothing from the target, so it works on a foreign-arch
# rootfs from the image builder. The packages in zynq-packages.txt must be installed in ROOTFS too
# (before or after this; nothing here depends on the order).
#   /etc/default/zynqram                    ZACCEL_SWAP_MB=256 (an existing file is kept)
#   /etc/nbd-server/conf.d/zynqram.conf     export [zynqram], TCP 10809
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
