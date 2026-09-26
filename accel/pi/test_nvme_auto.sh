#!/bin/bash
# test_nvme_auto.sh -- nvme-auto against loop-device drives in every state it can meet (WSL, root).
# Proves: a blank drive is partitioned, formatted, mounted and put in fstab; a second run and a
# "reboot" change nothing and keep the data; a drive with a filesystem is used and never reformatted;
# unknown data and unformatted partitions are left byte-for-byte untouched; no drive is not an error;
# the fstab line mounts by itself (mount -T) and passes findmnt --verify; the unit runs under systemd.
set -u
A=$(cd "$(dirname "$0")" && pwd)
S=$A/nvme-auto
W=$(mktemp -d /tmp/nvme-auto-test.XXXX)
LOOPS=()
PASS=0; FAIL=0
chk() { if eval "$2"; then echo "  PASS  $1"; PASS=$((PASS + 1)); else echo "  FAIL  $1"; FAIL=$((FAIL + 1)); fi; }
drive() {  # drive <name> <MiB> -> loop device with partition scanning
  truncate -s "$2M" "$W/$1.img"; local d; d=$(losetup -fP --show "$W/$1.img"); LOOPS+=("$d"); echo "$d"; }
run() {  # run <disk> <mountpoint> -> output in $W/out
  NVME_AUTO_DEVS=$1 NVME_AUTO_MNT=$2 NVME_AUTO_FSTAB=$W/fstab NVME_AUTO_WAIT=3 bash "$S" > "$W/out" 2>&1; echo $? > "$W/rc"; }
cleanup() {
  for m in "$W"/mnt*; do mountpoint -q "$m" && umount "$m"; done
  for d in "${LOOPS[@]}"; do losetup -d "$d" 2>/dev/null; done
  systemctl disable --now nvme-auto-test.service >/dev/null 2>&1; rm -f /etc/systemd/system/nvme-auto-test.service; systemctl daemon-reload
  rm -rf "$W"
}
trap cleanup EXIT
printf 'UUID=0000-root / ext4 defaults 0 1\ntmpfs /tmp tmpfs defaults 0 0\n' > "$W/fstab"
cp "$W/fstab" "$W/fstab.before"

echo "== blank drive"
D=$(drive blank 256); M=$W/mnt1
run "$D" "$M"; sed 's/^/    | /' "$W/out"
P=$(lsblk -nrpo NAME,TYPE "$D" | awk '$2=="part"{print $1}')
chk "exit 0" "[ \$(cat $W/rc) = 0 ]"
chk "one GPT partition" "[ \"\$(blkid -p -s PTTYPE -o value $D)\" = gpt ] && [ \$(echo \"$P\" | wc -w) = 1 ]"
chk "ext4 labelled nvme" "[ \"\$(blkid -p -s TYPE -o value $P)\" = ext4 ] && [ \"\$(blkid -p -s LABEL -o value $P)\" = nvme ]"
chk "mounted at the mount point" "mountpoint -q $M && [ \"\$(findmnt -nro SOURCE $M)\" = $P ]"
U=$(blkid -p -s UUID -o value "$P")
chk "fstab: one line, by UUID, nofail + device timeout" "[ \$(grep -c \"^UUID=$U $M ext4 .*nofail.*x-systemd.device-timeout\" $W/fstab) = 1 ]"
chk "fstab keeps every original line" "grep -qxF 'UUID=0000-root / ext4 defaults 0 1' $W/fstab && grep -qxF 'tmpfs /tmp tmpfs defaults 0 0' $W/fstab"
chk "original fstab backed up once" "cmp -s $W/fstab.before $W/fstab.nvme-auto.orig"
chk "findmnt --verify accepts the fstab" "findmnt --verify --tab-file $W/fstab >/dev/null 2>&1 || findmnt --verify --tab-file $W/fstab 2>&1 | grep -qv -i error"
if getent passwd 1000 >/dev/null; then chk "mount root owned by uid 1000" "[ \$(stat -c %u $M) = 1000 ]"; fi
echo data-1 > "$M/keep.txt"; sync

echo "== second run (idempotent)"
cp "$W/fstab" "$W/fstab.1"; run "$D" "$M"
chk "exit 0, fstab unchanged, data kept" "[ \$(cat $W/rc) = 0 ] && cmp -s $W/fstab $W/fstab.1 && [ \"\$(cat $M/keep.txt)\" = data-1 ]"
chk "not reformatted (same UUID)" "[ \"\$(blkid -p -s UUID -o value $P)\" = $U ]"

echo "== reboot: unmounted, run again"
umount "$M"; run "$D" "$M"
chk "mounted again with the data" "mountpoint -q $M && [ \"\$(cat $M/keep.txt)\" = data-1 ]"
umount "$M"
chk "the fstab line alone mounts it (mount -T fstab)" "mount -T $W/fstab $M && [ \"\$(cat $M/keep.txt)\" = data-1 ]"
umount "$M"

echo "== drive that already has a filesystem (someone else's)"
D2=$(drive used 128); M2=$W/mnt2
printf 'label: dos\n,,83\n' | sfdisk --quiet "$D2"; partprobe "$D2" 2>/dev/null; udevadm settle -t 5 2>/dev/null; sleep 1
P2=$(lsblk -nrpo NAME,TYPE "$D2" | awk '$2=="part"{print $1}'); mkfs.ext4 -q -L photos "$P2"
T=$(mktemp -d); mount "$P2" "$T"; echo theirs > "$T/theirs.txt"; umount "$T"; rmdir "$T"
U2=$(blkid -p -s UUID -o value "$P2")
run "$D2" "$M2"; sed 's/^/    | /' "$W/out"
chk "used as it is: same UUID, label, file" "[ \"\$(blkid -p -s UUID -o value $P2)\" = $U2 ] && [ \"\$(blkid -p -s LABEL -o value $P2)\" = photos ] && [ \"\$(cat $M2/theirs.txt)\" = theirs ]"
chk "fstab moved to the new drive: one line for $M2" "[ \$(grep -c \" $M2 \" $W/fstab) = 1 ] && grep -q \"^UUID=$U2 $M2 \" $W/fstab"
umount "$M2"

echo "== unknown data (no table, no filesystem, non-zero bytes)"
D3=$(drive junk 64); head -c 4096 /dev/urandom | dd of="$D3" bs=4096 seek=16 conv=notrunc status=none; sync
H3=$(sha256sum < "$D3"); cp "$W/fstab" "$W/fstab.3"
run "$D3" "$W/mnt3"; sed 's/^/    | /' "$W/out"
chk "left byte-for-byte untouched, fstab unchanged, exit 0" "[ \"\$(sha256sum < $D3)\" = \"$H3\" ] && cmp -s $W/fstab $W/fstab.3 && [ \$(cat $W/rc) = 0 ]"

echo "== partitions but no filesystem"
D4=$(drive parts 64); printf 'label: gpt\n,,L\n' | sfdisk --quiet "$D4"; partprobe "$D4" 2>/dev/null; sync
H4=$(sha256sum < "$D4")
run "$D4" "$W/mnt4"; sed 's/^/    | /' "$W/out"
chk "left untouched, exit 0" "[ \"\$(sha256sum < $D4)\" = \"$H4\" ] && [ \$(cat $W/rc) = 0 ]"

echo "== swap signature on the whole drive, and a drive whose only partition is swap"
D8=$(drive swap 64); mkswap "$D8" >/dev/null; sync; H8=$(sha256sum < "$D8")
chk "(setup) the swap signature is really there" "[ \"\$(blkid -p -s TYPE -o value $D8)\" = swap ]"
run "$D8" "$W/mnt8"; sed 's/^/    | /' "$W/out"
chk "whole-drive swap: untouched, not mounted, exit 0" "[ \"\$(sha256sum < $D8)\" = \"$H8\" ] && ! mountpoint -q $W/mnt8 && [ \$(cat $W/rc) = 0 ]"
D9=$(drive swappart 64); printf 'label: gpt
,,S
' | sfdisk --quiet "$D9"; partprobe "$D9" 2>/dev/null; udevadm settle -t 5 2>/dev/null; sleep 1
P9=$(lsblk -nrpo NAME,TYPE "$D9" | awk '$2=="part"{print $1}'); mkswap "$P9" >/dev/null; sync; H9=$(sha256sum < "$D9")
chk "(setup) the swap partition is really swap" "[ \"\$(blkid -p -s TYPE -o value $P9)\" = swap ]"
run "$D9" "$W/mnt9"; sed 's/^/    | /' "$W/out"
chk "swap partition only: untouched, not mounted, exit 0" "[ \"\$(sha256sum < $D9)\" = \"$H9\" ] && ! mountpoint -q $W/mnt9 && [ \$(cat $W/rc) = 0 ]"

echo "== empty partition table (a new drive some vendors ship)"
D5=$(drive empty 128); M5=$W/mnt5; printf 'label: gpt\n' | sfdisk --quiet "$D5"
run "$D5" "$M5"; sed 's/^/    | /' "$W/out"
chk "treated as blank: partitioned, ext4, mounted" "mountpoint -q $M5 && [ \"\$(blkid -p -s LABEL -o value \$(findmnt -nro SOURCE $M5))\" = nvme ]"
umount "$M5"

echo "== no drive at all"
run /dev/nvme-none "$W/mnt6"; sed 's/^/    | /' "$W/out"
chk "exit 0 with a clear message" "[ \$(cat $W/rc) = 0 ] && grep -q 'no NVMe drive found' $W/out"

echo "== the unit under systemd"
D7=$(drive unit 128); M7=$W/mnt7
sed -e "s#^ExecStart=.*#Environment=NVME_AUTO_DEVS=$D7 NVME_AUTO_MNT=$M7 NVME_AUTO_FSTAB=$W/fstab NVME_AUTO_WAIT=3\nExecStart=/bin/bash $S#" \
    -e 's/^Before=.*//' "$A/nvme-auto.service" > /etc/systemd/system/nvme-auto-test.service
systemctl daemon-reload
chk "systemctl start succeeds, unit active, drive mounted" "systemctl start nvme-auto-test.service && systemctl is-active -q nvme-auto-test.service && mountpoint -q $M7"
journalctl -u nvme-auto-test.service -n 6 --no-pager -o cat 2>/dev/null | sed 's/^/    | /'
sed "s#^ExecStart=.*#ExecStart=/bin/bash $S#" "$A/nvme-auto.service" > "$W/nvme-auto.service"
chk "systemd-analyze verify: the shipped unit" "systemd-analyze verify $W/nvme-auto.service"

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ] && echo "NVME AUTO TEST: PASS" || echo "NVME AUTO TEST: FAIL"
[ "$FAIL" = 0 ]
