#!/bin/bash
# test_nvme_claim.sh -- the Pi's real NVMe, rebuilt as a loop device, and the exact command the owner runs
# to hand it to the Pi. (WSL, root.)   bash /mnt/d/espicpc/accel/pi/test_nvme_claim.sh
#
# The drive, as read back from the Pi's own log (2026-09-26, memory pi-first-boot-measured): a used laptop
# drive, Samsung MZVLB256HAHQ, GPT with a 256 MiB vfat EFI System Partition next to another OS's data.
# Rebuilt here at 1/64 scale with the Windows layout such drives ship with: ESP (vfat) + MSR (no
# filesystem) + basic data (NTFS) + recovery (NTFS), with the ESP mounted as an earlier nvme-auto did.
#   1. nvme-auto must leave it untouched (none of it is storage) and print the claim command;
#   2. the claim command, run as given (the unmount first), must leave one ext4 partition labelled
#      "nvme", one fstab line by UUID, and the drive mounted -- with nothing of the old layout left.
set -uo pipefail
H=$(cd "$(dirname "$0")" && pwd)
W=/root/nvme-claim-test; rm -rf "$W"; mkdir -p "$W"
fail=0; ok() { echo "  PASS  $*"; }; bad() { echo "  FAIL  $*"; fail=1; }
command -v mkntfs >/dev/null || apt-get install -y -q ntfs-3g >/dev/null 2>&1
command -v mkntfs >/dev/null || { echo "FAIL: mkntfs unavailable (ntfs-3g)"; exit 1; }

truncate -s 4G "$W/drive.img"
DEV=$(losetup -fP --show "$W/drive.img") || { echo "FAIL: losetup"; exit 1; }
cleanup() { umount "$W"/mnt* 2>/dev/null; umount "$W"/esp 2>/dev/null; losetup -d "$DEV" 2>/dev/null; }
trap cleanup EXIT
sfdisk --quiet "$DEV" <<EOF
label: gpt
size=256MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI system partition"
size=16MiB,  type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE, name="Microsoft reserved partition"
size=3200MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="Basic data partition"
type=DE94BBA4-06D1-4D40-A16A-BFD50179D6AC, name="Recovery"
EOF
partprobe "$DEV" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 -n SYSTEM "${DEV}p1" >/dev/null
mkntfs -Q -L Windows "${DEV}p3" >/dev/null 2>&1
mkntfs -Q -L Recovery "${DEV}p4" >/dev/null 2>&1
mkdir -p "$W/esp" && mount "${DEV}p1" "$W/esp" && echo "laptop files" > "$W/esp/bootmgfw.efi"
echo "-- the drive before:"; lsblk -o NAME,SIZE,FSTYPE,PARTTYPENAME,LABEL "$DEV" | sed 's/^/   /'
touch "$W/fstab"
export NVME_AUTO_DEVS=$DEV NVME_AUTO_MNT=$W/mnt NVME_AUTO_FSTAB=$W/fstab NVME_AUTO_WAIT=2

echo "-- 1. nvme-auto on the laptop drive"
out=$(bash "$H/nvme-auto" 2>&1); echo "$out" | sed 's/^/   /'
echo "$out" | grep -q "left untouched" && ok "drive with another OS's data left untouched" || bad "nvme-auto did not leave it alone"
echo "$out" | grep -q "ERASES EVERYTHING" && ok "the claim command is printed" || bad "no claim command printed"
echo "$out" | grep -q "sudo umount ${DEV}p\* ; sudo wipefs -a $DEV" && ok "the printed command unmounts first" || bad "printed command lacks the unmount"
[ -f "$W/esp/bootmgfw.efi" ] && ok "the laptop's files are still there" || bad "data changed by nvme-auto"

echo "-- 2. the owner's command, exactly as given, with this drive's name"
# what the owner types on the Pi (with /dev/nvme0n1):
#   sudo umount /dev/nvme0n1p* ; sudo wipefs -a /dev/nvme0n1 && sudo dd if=/dev/zero of=/dev/nvme0n1 bs=1M count=1 conv=fsync && sudo systemctl restart nvme-auto
umount ${DEV}p* 2>/dev/null; wipefs -a "$DEV" >/dev/null && dd if=/dev/zero of="$DEV" bs=1M count=1 conv=fsync status=none \
  && out=$(bash "$H/nvme-auto" 2>&1)
echo "$out" | sed 's/^/   /'
echo "-- the drive after:"; lsblk -o NAME,SIZE,FSTYPE,PARTTYPENAME,LABEL "$DEV" | sed 's/^/   /'
n=$(lsblk -nrpo NAME,TYPE "$DEV" | awk '$2=="part"' | wc -l)
p=$(lsblk -nrpo NAME,TYPE "$DEV" | awk '$2=="part"{print $1}' | head -1)
[ "$n" = 1 ] && ok "one partition" || bad "$n partitions"
[ "$(blkid -p -s TYPE -o value "$p" 2>/dev/null)" = ext4 ] && [ "$(blkid -p -s LABEL -o value "$p" 2>/dev/null)" = nvme ] && ok "ext4 labelled nvme" || bad "not ext4/nvme"
grep -q "^UUID=$(blkid -p -s UUID -o value "$p") $W/mnt ext4 " "$W/fstab" && ok "one fstab line by UUID" || bad "fstab: $(cat "$W/fstab")"
mountpoint -q "$W/mnt" && ok "mounted: $(df -h --output=size,avail "$W/mnt" | tail -1 | sed 's/  */ /g')" || bad "not mounted"
echo "$out" | grep -q "^nvme-auto: ready" && ok "nvme-auto reports ready" || bad "no ready line"
[ $fail = 0 ] && echo "NVME CLAIM TEST: PASS" || echo "NVME CLAIM TEST: FAIL"
exit $fail
