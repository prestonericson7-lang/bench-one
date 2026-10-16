#!/bin/bash
# test_nvme_pi_drive.sh -- nvme-auto against the Orange Pi's REAL drive, as its own logs recorded it
# (2026-10-05 card read: kernel " nvme0n1: p1"; nvme-auto "ready: /dev/nvme0n1p1 256M 97M"; fstab
# "UUID=2ECC-05B7 /mnt/nvme vfat"): ONE partition holding a 256 MiB FAT filesystem with ~159 MB on it, the
# rest of the drive (238.5 GB there, 4 GiB here) not partitioned, and that partition mounted as the NVMe
# at boot by the stale fstab line. What the logs cannot say -- the table type and the partition's type --
# is covered by three variants. Runs the EXACT bytes that go onto the card (out/nvme-auto.inplace).
# (WSL, root.)   bash /mnt/d/espicpc/accel/pi/test_nvme_pi_drive.sh
# PI_IMG=<the Pi's card image>: nvme-auto runs inside that image's own root filesystem (an overlay; the
# image is mounted read-only) with ITS aarch64 util-linux / e2fsprogs / parted under qemu-user. The Pi runs
# Ubuntu 26.04 with util-linux 2.41.3, this host 22.04 with 2.37.2, and nvme-auto parses sfdisk's output.
#   PI_IMG=/root/opi/opi4pro-accel.img bash /mnt/d/espicpc/accel/pi/test_nvme_pi_drive.sh
set -uo pipefail
H=$(cd "$(dirname "$0")" && pwd)
fail=0; ok() { echo "  PASS  $*"; }; bad() { echo "  FAIL  $*"; fail=1; }
CARD=${NVME_CARD:-$H/out/nvme-auto.inplace}   # NVME_CARD: another file (a negative control)
[ "$(stat -c %s "$CARD")" = 7959 ] && ok "card file is exactly 7959 bytes" || { bad "card file is $(stat -c %s "$CARD") bytes"; exit 1; }
bash -n "$CARD" && ok "card file parses" || bad "card file does not parse"
# IW: the work directory as nvme-auto sees it (the paths it writes into fstab); W: the same directory from here
IW=/root/nvme-pi-drive; P=; R=
if [ -n "${PI_IMG:-}" ]; then
  P=$(mktemp -d /tmp/nvme-pi-root.XXXX); R=$P/root; mkdir -p "$P/lower" "$P/upper" "$P/work" "$R"
  mount -o ro,loop,offset=$((32 * 1024 * 1024)) "$PI_IMG" "$P/lower" || { echo "cannot mount $PI_IMG"; exit 1; }
  mount -t overlay overlay -o "lowerdir=$P/lower,upperdir=$P/upper,workdir=$P/work" "$R" || exit 1
  # rslave: unmounting the chroot's submounts must never propagate to WSL's own /dev, /proc, /sys
  for m in dev proc sys; do mount --rbind "/$m" "$R/$m" && mount --make-rslave "$R/$m"; done
  RUN=(chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin)
  cp "$CARD" "$R/tmp/nvme-auto.card"; ICARD=/tmp/nvme-auto.card
  echo "-- nvme-auto runs with the Pi's own tools: $(chroot "$R" /usr/bin/uname -m), $(grep '^PRETTY' "$R/etc/os-release" | cut -d= -f2), $(chroot "$R" /usr/sbin/sfdisk --version)"
else
  RUN=(env); ICARD=$CARD
fi
W=$R$IW; rm -rf "$W"; mkdir -p "$W"
DEV=
cleanup() { umount "$W/mnt" "$W/peek" 2>/dev/null; [ -n "$DEV" ] && losetup -d "$DEV" 2>/dev/null; DEV=; }
final() { cleanup
  if [ -n "$P" ]; then for m in dev proc sys; do mountpoint -q "$R/$m" && umount -R "$R/$m"; done
    mountpoint -q "$R" && umount "$R"; mountpoint -q "$P/lower" && umount "$P/lower"; rm -rf "$P"; fi; }
trap final EXIT

# drive LABEL TABLE PTYPE P1SIZE FSSIZE -- one partition, a FAT filesystem with files, mounted at $W/mnt
drive() {
  cleanup; rm -f "$W/drive.img"; truncate -s 4G "$W/drive.img"
  DEV=$(losetup -fP --show "$W/drive.img")
  local sz=""; [ -n "$4" ] && sz="size=$4, "                 # no size = to the end of the drive
  printf 'label: %s\nstart=2048, %stype=%s\n' "$2" "$sz" "$3" | sfdisk --quiet "$DEV" || { bad "test setup: sfdisk"; return 1; }
  partprobe "$DEV"; udevadm settle 2>/dev/null; sleep 1
  [ -b "${DEV}p1" ] || { bad "test setup: ${DEV}p1 is not a block device"; return 1; }
  mkfs.vfat -F 32 -n DATA -C "$W/fs.img" $(( $5 / 1024 )) >/dev/null 2>&1 || mkfs.vfat -F 32 -n DATA "${DEV}p1" >/dev/null
  if [ -f "$W/fs.img" ]; then dd if="$W/fs.img" of="${DEV}p1" bs=1M conv=fsync status=none; rm -f "$W/fs.img"; fi
  mkdir -p "$W/mnt" && mount "${DEV}p1" "$W/mnt"
  mkdir -p "$W/mnt/EFI/BOOT" "$W/mnt/stuff"
  head -c 150M /dev/urandom > "$W/mnt/stuff/blob.bin"; echo bootx64 > "$W/mnt/EFI/BOOT/BOOTX64.EFI"
  sync; (cd "$W/mnt" && find . -type f -exec sha256sum {} + | sort) > "$W/p1.sums"
  P1UUID=$(blkid -p -s UUID -o value "${DEV}p1")
  printf 'UUID=a81ee6f1 / ext4 defaults,noatime,commit=600,errors=remount-ro 0 1\ntmpfs /tmp tmpfs defaults,nosuid 0 0\n# nvme-auto: the NVMe drive\nUUID=%s %s vfat defaults,noatime,nofail,x-systemd.device-timeout=15s,x-gvfs-show,x-gvfs-name=NVMe 0 2\n' "$P1UUID" "$IW/mnt" > "$W/fstab"
  rm -f "$W/nvme-auto.log"
}
run() { "${RUN[@]}" NVME_AUTO_DEVS="$DEV" NVME_AUTO_MNT="$IW/mnt" NVME_AUTO_FSTAB="$IW/fstab" NVME_AUTO_WAIT=2 NVME_AUTO_LOG="$IW/nvme-auto.log" bash "$ICARD" 2>&1; }
p1_intact() {   # every file's sha256, read where p1 is mounted now, or from a read-only mount of it
  local m; m=$(findmnt -nro TARGET --source "${DEV}p1" 2>/dev/null | head -1)
  if [ -n "$m" ]; then (cd "$m" && find . -type f -exec sha256sum {} + | sort) > "$W/p1.after"
  else mkdir -p "$W/peek"; mount -o ro "${DEV}p1" "$W/peek" && (cd "$W/peek" && find . -type f -exec sha256sum {} + | sort) > "$W/p1.after"; umount "$W/peek"; fi
  [ -s "$W/p1.after" ] && cmp -s "$W/p1.sums" "$W/p1.after"; }

real_drive_case() {   # NAME TABLE PTYPE
  echo "-- $1"
  drive "$1" "$2" "$3" 256MiB 268435456
  out=$(run); echo "$out" | sed 's/^/   /'
  n=$(lsblk -nrpo NAME,TYPE "$DEV" | awk '$2=="part"' | wc -l)
  p2=$(lsblk -nrpo NAME,TYPE "$DEV" | awk '$2=="part"{print $1}' | sed -n 2p)
  [ "$n" = 2 ] && [ "$(blkid -p -s TYPE -o value "$p2")" = ext4 ] && [ "$(blkid -p -s LABEL -o value "$p2")" = nvme ] && ok "a new ext4 'nvme' partition in the free space" || bad "partitions after: $(lsblk -o NAME,SIZE,FSTYPE,LABEL "$DEV" | tr '\n' ' ')"
  [ "$(( $(lsblk -bdno SIZE "$p2") / 1048576 ))" -gt 3500 ] && ok "it has the free space: $(( $(lsblk -bdno SIZE "$p2") / 1048576 )) MiB" || bad "new partition size $(lsblk -bdno SIZE "$p2")"
  p1_intact && ok "the FAT partition's files are unchanged (sha256 of every file)" || bad "the FAT partition's files changed"
  [ "$(findmnt -nro SOURCE --target "$W/mnt")" = "$p2" ] && ok "the new partition is what is mounted as the NVMe" || bad "mounted: $(findmnt -nro SOURCE --target "$W/mnt")"
  grep -q "$P1UUID" "$W/fstab" && bad "the stale FAT line is still in fstab" || ok "the stale FAT line is gone"
  [ "$(grep -c " $IW/mnt " "$W/fstab")" = 1 ] && grep -q "^UUID=$(blkid -p -s UUID -o value "$p2") $IW/mnt ext4 " "$W/fstab" && grep -q "^tmpfs /tmp" "$W/fstab" && ok "fstab: one line for the NVMe, the new UUID, other lines kept" || bad "fstab: $(cat "$W/fstab")"
  grep -q "layout: table" "$W/nvme-auto.log" && grep -q "holds: EFI stuff" "$W/nvme-auto.log" && grep -q "free: sectors" "$W/nvme-auto.log" && ok "layout and the FAT partition's contents logged on the card" || bad "log: $(cat "$W/nvme-auto.log" 2>/dev/null | head -12)"
  out=$(run)
  [ "$(lsblk -nrpo NAME,TYPE "$DEV" | awk '$2=="part"' | wc -l)" = 2 ] && echo "$out" | grep -q "using existing ext4 filesystem on $p2" && ok "next boot: the same partition, nothing new" || bad "next boot: $(echo "$out" | tail -3 | tr '\n' ' ')"
}

real_drive_case "GPT, basic-data partition" gpt EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
real_drive_case "MBR, FAT32 (0x0c) partition" dos c
real_drive_case "GPT, EFI-typed partition" gpt C12A7328-F81F-11D2-BA4B-00A0C93EC93B

echo "-- a big existing data partition and a little free space: the existing one is used"
cleanup; rm -f "$W/drive.img"; truncate -s 4G "$W/drive.img"; DEV=$(losetup -fP --show "$W/drive.img")
printf 'label: gpt\nstart=2048, size=3500MiB, type=L\n' | sfdisk --quiet "$DEV"; partprobe "$DEV"; sleep 1
mkfs.ext4 -q -L data "${DEV}p1"; : > "$W/fstab"; rm -f "$W/nvme-auto.log"
out=$(run)
[ "$(lsblk -nrpo NAME,TYPE "$DEV" | awk '$2=="part"' | wc -l)" = 1 ] && echo "$out" | grep -q "using existing ext4 filesystem on ${DEV}p1" && ok "existing 3.5 GiB ext4 used, no new partition" || bad "$(echo "$out" | tail -3 | tr '\n' ' ')"

echo "-- one partition over the whole drive, a small FAT inside it: no free space, used as it is"
drive "whole" gpt EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 "" 268435456
out=$(run)
[ "$(lsblk -nrpo NAME,TYPE "$DEV" | awk '$2=="part"' | wc -l)" = 1 ] && echo "$out" | grep -q "using existing vfat filesystem on ${DEV}p1" && ok "no free space: the FAT partition used as it is, nothing created" || bad "$(echo "$out" | tail -3 | tr '\n' ' ')"
p1_intact && ok "its files unchanged" || bad "files changed"
grep -q "layout: table" "$W/nvme-auto.log" && ok "layout logged" || bad "no layout logged"

# an EFI System Partition over the whole drive is another OS's boot partition, never storage: left alone,
# the stale line taken back out of fstab, nothing mounted. The MBR case is the one the Pi's own sfdisk
# reports as "ef" (no 0x) -- the first version compared only "0xef" and would have mounted it.
esp_whole_case() {   # NAME TABLE PTYPE
  echo "-- $1 over the whole drive: not storage"
  drive "$1" "$2" "$3" "" 268435456
  out=$(run); echo "$out" | sed 's/^/   /'
  [ "$(lsblk -nrpo NAME,TYPE "$DEV" | awk '$2=="part"' | wc -l)" = 1 ] && echo "$out" | grep -q "none of them storage" && ok "left untouched, reported as not storage" || bad "$(echo "$out" | tail -3 | tr '\n' ' ')"
  p1_intact && ok "its files unchanged" || bad "files changed"
  mountpoint -q "$W/mnt" && bad "something is still mounted as the NVMe: $(findmnt -nro SOURCE --target "$W/mnt")" || ok "nothing mounted as the NVMe"
  grep -q " $IW/mnt " "$W/fstab" && bad "fstab still has a line for the NVMe" || ok "the stale fstab line taken out"
  grep -q "^tmpfs /tmp" "$W/fstab" && ok "the other fstab lines kept" || bad "fstab: $(cat "$W/fstab")"
  echo "$out" | grep -q "sudo umount ${DEV}p\* ; sudo wipefs -a $DEV" && ok "the erase command is printed, not run" || bad "no erase command"
}
esp_whole_case "GPT, EFI System Partition" gpt C12A7328-F81F-11D2-BA4B-00A0C93EC93B
esp_whole_case "MBR, EFI (0xef) partition" dos ef

[ $fail = 0 ] && echo "NVME PI DRIVE TEST: PASS" || echo "NVME PI DRIVE TEST: FAIL"
exit $fail
