#!/bin/bash
# build_rootfs.sh -- Debian bookworm armhf root filesystem for the PZ7020-StarLite (inside WSL as root).
#   debootstrap --foreign + qemu-arm-static second stage; ssh + serial console + the tools the node needs.
#     wsl -d Ubuntu-22.04 -u root -- bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/build_rootfs.sh
set -euo pipefail
REPO=${REPO:-/mnt/d/espicpc}
OUT=${OUT:-$REPO/hardware/pz7020-starlite/linux/out}
ROOT=${ROOT:-/root/zynq/rootfs}
HOSTNAME=zynq
ROOTPW=${ROOTPW:-zynq}          # console/ssh root password; change on first login
rm -rf "$ROOT"; mkdir -p "$ROOT" "$OUT"
debootstrap --arch=armhf --foreign --variant=minbase \
  --include=systemd-sysv,udev,openssh-server,ifupdown,isc-dhcp-client,iproute2,iputils-ping,ethtool,net-tools,\
nano,less,htop,usbutils,pciutils,i2c-tools,python3,python3-serial,python3-spidev,ca-certificates,wget,curl,rsync,\
mtd-utils,u-boot-tools,device-tree-compiler,kmod,sudo,locales,dbus,tzdata \
  bookworm "$ROOT" http://deb.debian.org/debian
cp /usr/bin/qemu-arm-static "$ROOT/usr/bin/"
chroot "$ROOT" /debootstrap/debootstrap --second-stage

# --- identity, console, network, ssh ---
echo "$HOSTNAME" > "$ROOT/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\t%s\n' "$HOSTNAME" > "$ROOT/etc/hosts"
cat > "$ROOT/etc/fstab" <<EOF
/dev/mmcblk0p2  /      ext4  defaults,noatime  0 1
/dev/mmcblk0p1  /boot  vfat  defaults          0 2
EOF
cat > "$ROOT/etc/network/interfaces" <<EOF
auto lo
iface lo inet loopback
auto eth0
iface eth0 inet dhcp
EOF
chroot "$ROOT" bash -c "echo root:$ROOTPW | chpasswd"
sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin yes/' "$ROOT/etc/ssh/sshd_config"
chroot "$ROOT" systemctl enable serial-getty@ttyPS0.service ssh >/dev/null 2>&1 || true
echo "en_US.UTF-8 UTF-8" > "$ROOT/etc/locale.gen"; chroot "$ROOT" locale-gen >/dev/null 2>&1 || true
mkdir -p "$ROOT/boot" "$ROOT/lib/firmware"
# kernel modules from build_kernel.sh
[ -f "$OUT/modules.tar.gz" ] && tar xzf "$OUT/modules.tar.gz" -C "$ROOT/"
# PL bitstreams the fpga_manager can load at runtime: /sys/class/fpga_manager/fpga0/firmware
for b in "$REPO"/hardware/pz7020-starlite/fan/build/fan_top.bit "$REPO"/firmware/rtlsdr-pentest/fpga/openxc7/build/sdr_accel_zynq_top.bit; do
  [ -f "$b" ] && python3 "$REPO/hardware/pz7020-starlite/linux/bit2bin.py" "$b" "$ROOT/lib/firmware/$(basename "${b%.bit}").bin"
done
rm -f "$ROOT/usr/bin/qemu-arm-static"
du -sh "$ROOT"
echo "ROOTFS DONE: $ROOT (root password: $ROOTPW)"
