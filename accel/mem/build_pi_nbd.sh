#!/bin/bash
# build_pi_nbd.sh -- the Orange Pi's official kernel (6.6.98-sun60iw2) is built WITHOUT the NBD
# driver (CONFIG_BLK_DEV_NBD is not set in /boot/config-6.6.98-sun60iw2), so the Zynq's RAM export
# has no block device to arrive at. This builds nbd.ko for exactly that kernel from the vendor's own
# source (orangepi-xunlong/linux-orangepi, branch orange-pi-6.6-sun60iw2) and the image's own config.
# The kernel has MODVERSIONS and MODULE_SIG off, so a module from the same source + config loads.
#   Run inside WSL as root. Output: accel/mem/pi-kmod/6.6.98-sun60iw2/nbd.ko
set -euo pipefail
REPO=/mnt/d/espicpc
IMG=${IMG:-/mnt/c/Users/Danie/Downloads/opi4pro/Orangepi4pro_1.1.0_ubuntu_resolute_desktop_xfce_linux6.6.98.img}
KREL=6.6.98-sun60iw2
SRC=/root/opi/linux-orangepi
OUTD=$REPO/accel/mem/pi-kmod/$KREL
mkdir -p /root/opi "$OUTD"
# The nbd.ko committed in accel/mem/pi-kmod/ was built from commit 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f
# (2026-09-17) of that branch -- its corresponding source (THIRD-PARTY-NOTICES.md).
if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 -b orange-pi-6.6-sun60iw2 https://github.com/orangepi-xunlong/linux-orangepi "$SRC"
fi
cd "$SRC"
# the running kernel's own config, read from the official image (read-only mount)
M=$(mktemp -d); mount -o ro,loop,offset=$((32*1024*1024)) "$IMG" "$M"
cp "$M/boot/config-$KREL" .config
MODS_SYMS=$(ls "$M/lib/modules/$KREL/" | tr '\n' ' ')
umount "$M"; rmdir "$M"
echo "image modules dir has: $MODS_SYMS"
./scripts/config --module BLK_DEV_NBD --disable LOCALVERSION_AUTO
touch .scmversion
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make -s olddefconfig
# the release string must be exactly the running kernel's
LV=""
for cand in "" "-sun60iw2"; do
  if [ "$(make -s LOCALVERSION="$cand" kernelrelease)" = "$KREL" ]; then LV=$cand; break; fi
done
[ "$(make -s LOCALVERSION="$LV" kernelrelease)" = "$KREL" ] || { echo "cannot reproduce kernelrelease $KREL (got $(make -s kernelrelease))"; exit 1; }
echo "kernelrelease $(make -s LOCALVERSION="$LV" kernelrelease) (LOCALVERSION='$LV')"
make -s LOCALVERSION="$LV" -j"$(nproc)" modules_prepare
# the driver alone, as an external module built from the in-tree source
X=/root/opi/nbd-ext; rm -rf "$X"; mkdir -p "$X"
cp drivers/block/nbd.c "$X/"
echo 'obj-m := nbd.o' > "$X/Makefile"
# No vmlinux here, so no Module.symvers: modpost cannot resolve symbols and would refuse. With
# MODVERSIONS off the kernel resolves them at load time by name, so build anyway -- and then PROVE
# every symbol the module needs is exported by the official kernel, from the image's own Image.
make -s LOCALVERSION="$LV" KBUILD_MODPOST_WARN=1 M="$X" modules 2>&1 | grep -v -E "undefined!|unresolved symbol" || true
[ -f "$X/nbd.ko" ] || { echo "nbd.ko not built"; exit 1; }
cp "$X/nbd.ko" "$OUTD/nbd.ko"
modinfo "$OUTD/nbd.ko" | grep -E "^(vermagic|depends|license|filename)"
M=$(mktemp -d); mount -o ro,loop,offset=$((32*1024*1024)) "$IMG" "$M"
cp "$M/boot/vmlinux-$KREL" /root/opi/Image           # the official kernel (arm64 Image)
cp "$M/boot/System.map-$KREL" /root/opi/System.map   # its symbol table: __ksymtab_X = X is exported
umount "$M"; rmdir "$M"
awk '$3 ~ /^__ksymtab_/ {sub(/^__ksymtab_/, "", $3); print $3}' /root/opi/System.map | sort -u > /root/opi/exported.txt
aarch64-linux-gnu-nm -u "$OUTD/nbd.ko" | awk '{print $2}' | sort -u > /root/opi/nbd-needs.txt
missing=$(comm -23 /root/opi/nbd-needs.txt /root/opi/exported.txt)
echo "nbd.ko needs $(wc -l < /root/opi/nbd-needs.txt) kernel symbols; the official kernel exports $(wc -l < /root/opi/exported.txt)"
if [ -n "$missing" ]; then echo "NOT exported by the official kernel:"; echo "$missing"; exit 1; fi
echo "every symbol nbd.ko needs is exported by the official kernel (System.map-$KREL)"
# the loader compares this string with the running kernel's, byte for byte
{ strings /root/opi/Image | grep -m1 -E "^$KREL SMP" | sed 's/^/kernel vermagic: /'; } || true
aarch64-linux-gnu-strip --strip-debug "$OUTD/nbd.ko"
(cd "$OUTD" && sha256sum nbd.ko | tee nbd.ko.sha256)
echo "NBD MODULE DONE: $OUTD/nbd.ko"
