#!/bin/bash
# test_stage_pi_card.sh -- stage the bundle into a writable overlay of the OFFICIAL Orange Pi image's
# rootfs (the image file is mounted read-only and never written), then check the result from inside
# with the Pi's own aarch64 userspace under qemu-user: the unit is enabled as its systemd sees it, the
# offline .debs install against the image's real libraries, and nvme-auto works with the image's own
# util-linux / e2fsprogs (newer than this host's). Also: staging refuses a root that is not the Pi's.
# WSL, root.
set -u
IMG=${OPI_IMG:-/mnt/c/Users/Danie/Downloads/opi4pro/Orangepi4pro_1.1.0_ubuntu_resolute_desktop_xfce_linux6.6.98.img}
A=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /tmp/stage-test.XXXX)
R=$W/root
PASS=0; FAIL=0; LOOPS=()
chk() { if eval "$2"; then echo "  PASS  $1"; PASS=$((PASS + 1)); else echo "  FAIL  $1"; FAIL=$((FAIL + 1)); fi; }
cleanup() {
  # the binds are rslave (below), so these unmounts can never reach the host's own /dev, /proc, /sys
  for m in nvtest nvtest2 nvtest3; do mountpoint -q "$R/mnt/$m" && umount "$R/mnt/$m"; done
  for m in dev proc sys; do mountpoint -q "$R/$m" && umount -R "$R/$m"; done
  for m in "$R" "$W/lower"; do mountpoint -q "$m" && umount "$m"; done
  for d in "${LOOPS[@]}"; do losetup -d "$d" 2>/dev/null; done
  rm -rf "$W"
}
trap cleanup EXIT
[ -f "$IMG" ] || { echo "no image at $IMG"; exit 1; }
mkdir -p "$W/lower" "$W/upper" "$W/work" "$R"
mount -o ro,loop,offset=$((32 * 1024 * 1024)) "$IMG" "$W/lower" || exit 1
mount -t overlay overlay -o "lowerdir=$W/lower,upperdir=$W/upper,workdir=$W/work" "$R" || exit 1

echo "== staging"
bash "$A/stage_pi_card.sh" root "$R" > "$W/stage.log" 2>&1; rc=$?
sed 's/^/    | /' "$W/stage.log"
chk "stage exit 0, every file read back identical" "[ $rc = 0 ] && grep -q 'read back: [0-9]* files, 0 problems' $W/stage.log"
chk "unit enabled: multi-user.target.wants link" "[ \"\$(readlink $R/etc/systemd/system/multi-user.target.wants/accel-firstboot.service)\" = /etc/systemd/system/accel-firstboot.service ]"
bad=$(cd "$W/upper" && find . \( -type f -o -type l \) | grep -vE '^\./opt/accel/bundle/|^\./etc/systemd/system/(accel-firstboot\.service|multi-user\.target\.wants/accel-firstboot\.service)$' || true)
chk "wrote nothing outside /opt/accel/bundle and the unit" "[ -z \"$bad\" ]"
[ -n "$bad" ] && echo "$bad" | head | sed 's/^/    unexpected: /'
chk "bundle carries the offline .debs, nvme-auto and the first-boot script" "ls $R/opt/accel/bundle/pi/debs/*.deb $R/opt/accel/bundle/pi/nvme-auto $R/opt/accel/bundle/pi/accel-firstboot.sh >/dev/null"

echo "== refusals"
mkdir -p "$W/fake/etc"; echo VERSION_CODENAME=jammy > "$W/fake/etc/os-release"
chk "refuses a root that is not the Pi's (and writes nothing there)" "! bash $A/stage_pi_card.sh root $W/fake >/dev/null 2>&1 && [ ! -e $W/fake/opt ]"

echo "== inside the Pi's own userspace (aarch64 under qemu-user)"
# rslave: without it, unmounting the chroot's /dev/pts or /sys submounts propagates to the host and takes
# WSL's own mounts with them (measured: it did, 2026-09-25 -- every new wsl session failed until --shutdown)
for m in dev proc sys; do mount --rbind "/$m" "$R/$m" && mount --make-rslave "$R/$m"; done
in_pi() { chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin "$@"; }
chk "chroot runs aarch64 (uname -m inside)" "[ \"\$(in_pi /usr/bin/uname -m)\" = aarch64 ]"
chk "the Pi's systemctl sees the unit enabled" "[ \"\$(in_pi systemctl is-enabled accel-firstboot.service 2>/dev/null)\" = enabled ]"
chk "the Pi's bash parses install_pi.sh, accel-firstboot.sh, nvme-auto" "in_pi bash -n /opt/accel/bundle/install_pi.sh && in_pi bash -n /opt/accel/bundle/pi/accel-firstboot.sh && in_pi bash -n /opt/accel/bundle/pi/nvme-auto"
# The real first boot (2026-09-26): orangepi-firstrun's dpkg-reconfigure held the debconf database while the
# install ran, nbd-client's postinst died on it and stayed half-configured, and the initramfs trigger rebuilt
# /boot/initrd.img. Same here: hold config.dat exactly as debconf does (flock) for 25 s during the install.
IB=$R/boot/initrd.img-6.6.98-sun60iw2; UB=$R/boot/uInitrd-6.6.98-sun60iw2; CF=$R/etc/initramfs-tools/update-initramfs.conf
HB=$(cat "$IB" "$UB" | sha256sum); HC=$(sha256sum < "$CF")
in_pi perl -e 'use Fcntl qw(:flock); open(F, "+<", "/var/cache/debconf/config.dat") or die; flock(F, LOCK_EX) or die; sleep 25' &
LOCKER=$!; sleep 2
in_pi ACCEL_DPKG_PAUSE=5 ACCEL_DPKG_LOG=/tmp/accel-dpkg.log bash /opt/accel/bundle/pi/install_debs.sh /opt/accel/bundle/pi/debs > "$W/dpkg.log" 2>&1; rc=$?
wait $LOCKER 2>/dev/null
sed 's/^/    | /' "$W/dpkg.log"
chk "(setup) the debconf lock really bit: attempt 1 hit 'config.dat is locked'" "grep -q 'config.dat is locked by another process' $R/tmp/accel-dpkg.log"
chk "install_debs.sh waits it out: exit 0, both .debs 'install ok installed'" "[ $rc = 0 ] && in_pi dpkg -s nbd-client teensy-loader-cli 2>/dev/null | grep -c 'Status: install ok installed' | grep -qx 2"
chk "nbd-client's postinst ran to the end (its rcS.d link exists)" "ls $R/etc/rcS.d/ | grep -q nbd-client"
chk "/boot initrd + uInitrd byte-identical (no initramfs rebuild)" "[ \"\$(cat $IB $UB | sha256sum)\" = \"$HB\" ]"
chk "update-initramfs.conf restored exactly" "[ \"\$(sha256sum < $CF)\" = \"$HC\" ] && [ ! -e $CF.accel-save ]"
in_pi ACCEL_DPKG_LOG=/tmp/accel-dpkg.log bash /opt/accel/bundle/pi/install_debs.sh /opt/accel/bundle/pi/debs > "$W/dpkg2.log" 2>&1; rc=$?
chk "second run: nothing to do, exit 0" "[ $rc = 0 ] && grep -q 'already installed' $W/dpkg2.log"
chk "nbd-client runs (its libraries resolve)" "in_pi nbd-client --version 2>&1 | grep -qi 'nbd-client version\|This is nbd-client'"
chk "teensy_loader_cli runs (libusb-0.1 resolves)" "in_pi teensy_loader_cli --list-mcus 2>&1 | grep -qi teensy41"
chk "zaccel-bench starts on the Pi userspace" "in_pi /opt/accel/bundle/pi/out/aarch64/zaccel-bench --help 2>&1 | grep -q 'usage: zaccel-bench'"
chk "run_model and ppl start, built with the Zynq offload (--zaccel)" "in_pi /opt/accel/bundle/llm/out/aarch64/run_model 2>&1 | grep -q -- '--zaccel' && in_pi /opt/accel/bundle/llm/out/aarch64/ppl 2>&1 | grep -q -- '--zaccel'"

echo "== nvme-auto with the image's own util-linux / e2fsprogs"
truncate -s 256M "$W/nv.img"; D=$(losetup -fP --show "$W/nv.img"); LOOPS+=("$D")
printf 'UUID=%s / ext4 defaults 0 1\n' a81ee6f1-5c12-44f0-8e92-c28029cab13c > "$R/tmp/fstab.t"
in_pi NVME_AUTO_DEVS="$D" NVME_AUTO_MNT=/mnt/nvtest NVME_AUTO_FSTAB=/tmp/fstab.t NVME_AUTO_WAIT=3 bash /opt/accel/bundle/pi/nvme-auto > "$W/nv.log" 2>&1; rc=$?
sed 's/^/    | /' "$W/nv.log"
chk "blank drive: partitioned, ext4 'nvme', mounted, fstab line (Pi's tools)" "[ $rc = 0 ] && mountpoint -q $R/mnt/nvtest && grep -q ' /mnt/nvtest ext4 .*nofail' $R/tmp/fstab.t && [ \"\$(blkid -p -s LABEL -o value \$(findmnt -nro SOURCE $R/mnt/nvtest))\" = nvme ]"
umount "$R/mnt/nvtest"
truncate -s 64M "$W/junk.img"; J=$(losetup -fP --show "$W/junk.img"); LOOPS+=("$J")
head -c 4096 /dev/urandom | dd of="$J" bs=4096 seek=40 conv=notrunc status=none; sync; H=$(sha256sum < "$J")
in_pi NVME_AUTO_DEVS="$J" NVME_AUTO_MNT=/mnt/nvtest2 NVME_AUTO_FSTAB=/tmp/fstab.t NVME_AUTO_WAIT=3 bash /opt/accel/bundle/pi/nvme-auto > "$W/nv2.log" 2>&1
sed 's/^/    | /' "$W/nv2.log"
chk "unknown data: untouched (Pi's tools)" "[ \"\$(sha256sum < $J)\" = \"$H\" ] && grep -q 'unknown data' $W/nv2.log"
# the Pi's real drive: an EFI System Partition next to another OS's NTFS data -- the ESP is not storage
truncate -s 256M "$W/laptop.img"; L=$(losetup -fP --show "$W/laptop.img"); LOOPS+=("$L")
printf 'label: gpt\nsize=64M, type=U\ntype=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\n' | sfdisk --quiet "$L"; partprobe "$L" 2>/dev/null; udevadm settle -t 5 2>/dev/null; sleep 1
mkfs.vfat -n SYSTEM "${L}p1" >/dev/null && mkfs.ntfs -Q -F "${L}p2" >/dev/null 2>&1; sync; H=$(sha256sum < "$L")
cp "$R/tmp/fstab.t" "$W/fstab.t.before"
in_pi NVME_AUTO_DEVS="$L" NVME_AUTO_MNT=/mnt/nvtest3 NVME_AUTO_FSTAB=/tmp/fstab.t NVME_AUTO_WAIT=3 bash /opt/accel/bundle/pi/nvme-auto > "$W/nv3.log" 2>&1
sed 's/^/    | /' "$W/nv3.log"
chk "EFI partition + NTFS: untouched, nothing mounted, fstab unchanged (Pi's tools)" "[ \"\$(sha256sum < $L)\" = \"$H\" ] && ! mountpoint -q $R/mnt/nvtest3 && cmp -s $W/fstab.t.before $R/tmp/fstab.t && grep -q 'none of them storage' $W/nv3.log"

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ] && echo "STAGE PI CARD TEST: PASS" || echo "STAGE PI CARD TEST: FAIL"
[ "$FAIL" = 0 ]
