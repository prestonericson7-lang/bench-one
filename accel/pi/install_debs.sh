#!/bin/bash
# install_debs.sh DEBDIR -- install the bundled offline .debs (nbd-client, teensy-loader-cli) on the Pi.
# Called by install_pi.sh; exit 0 only when dpkg itself says both are "installed".
#
# Why this is more than "dpkg -i":
#   * On the first boot of the Orange Pi image, the vendor's orangepi-firstrun runs at the same time and
#     calls dpkg-reconfigure (libgdk-pixbuf, openssh-server), which holds the debconf database. nbd-client's
#     postinst opens debconf on its first line, so on the real Pi (2026-09-26) it died with "config.dat is
#     locked by another process" and stayed half-configured -- while its binary was already unpacked, so a
#     "command -v nbd-client" check called it installed. apt-daily/unattended-upgrades can hold the dpkg
#     lock the same way. So: retry for up to 5 minutes, and judge by dpkg's status, not by the binary.
#   * nbd-client ships initramfs hooks, so installing it rebuilds /boot/initrd.img and uInitrd (it did on
#     the Pi: 28.5 -> 31.4 MB). Nothing on the Pi boots from NBD, so the install leaves the boot files alone:
#     update_initramfs=no for the duration (update-initramfs -u then says "Not updating"), restored after.
set -uo pipefail
D=${1:?usage: install_debs.sh DEBDIR}
LOG=${ACCEL_DPKG_LOG:-/var/lib/accel/dpkg.log}
TRIES=${ACCEL_DPKG_TRIES:-30}
PAUSE=${ACCEL_DPKG_PAUSE:-10}
CONF=/etc/initramfs-tools/update-initramfs.conf
export DEBIAN_FRONTEND=noninteractive

[ "$(dpkg --print-architecture 2>/dev/null)" = arm64 ] || { echo "not an arm64 system: the bundled .debs are not for it"; exit 0; }
[ -f "$D/SHA256SUMS" ] || { echo "no $D/SHA256SUMS: no bundled .debs"; exit 0; }
(cd "$D" && sha256sum -c --quiet SHA256SUMS) || { echo "bundled .debs do not match $D/SHA256SUMS -- not installing them"; exit 1; }
mkdir -p "$(dirname "$LOG")"

installed() { [ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null)" = installed ]; }
need=()
installed nbd-client || need+=("$D"/nbd-client_*.deb)
installed teensy-loader-cli || need+=("$D"/teensy-loader-cli_*.deb)
[ ${#need[@]} = 0 ] && { echo "nbd-client and teensy-loader-cli already installed"; exit 0; }

restore=
if [ -f "$CONF" ] && grep -q '^update_initramfs=yes' "$CONF"; then
  cp -p "$CONF" "$CONF.accel-save" && sed -i 's/^update_initramfs=yes$/update_initramfs=no/' "$CONF" && restore=1
fi
trap '[ -n "$restore" ] && mv -f "$CONF.accel-save" "$CONF"' EXIT

for try in $(seq "$TRIES"); do
  echo "=== dpkg -i attempt $try, $(date)" >> "$LOG"
  if dpkg -i "${need[@]}" >> "$LOG" 2>&1 && installed nbd-client && installed teensy-loader-cli; then
    echo "installed from the bundle: ${need[*]##*/}$([ "$try" -gt 1 ] && echo " (attempt $try: dpkg/debconf was busy before)")"
    exit 0
  fi
  if [ "$try" = 1 ]; then
    echo "dpkg -i did not finish (another package tool busy?) -- retrying every ${PAUSE}s, up to $TRIES attempts. $LOG:"
    tail -4 "$LOG" | sed 's/^/  /'
  fi
  sleep "$PAUSE"
done
echo "dpkg -i still failing after $TRIES attempts -- $LOG:"; tail -8 "$LOG" | sed 's/^/  /'
exit 1
