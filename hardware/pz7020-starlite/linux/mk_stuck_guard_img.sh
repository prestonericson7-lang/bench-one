#!/bin/bash
# mk_stuck_guard_img.sh -- a QEMU test image whose PL guard hangs inside its first PL read, the way it would on
# a board where FCLK0, the level shifters and the reset all check fine and the read still never comes back.
# Starts from qemu_plcheck_test.sh's B image (today's boot files, plus U-Boot's after-load register writes, so
# the guard's checks pass and it goes on to read). The only change: the guard's PEEK child sleeps before it
# reads. Everything else -- the guard's checks, its synced "reading 0x..." line, the units waiting on it,
# systemd's console -- is the real image. Used by test_driver_e2e.sh. Run in WSL as root:
#     bash mk_stuck_guard_img.sh            -> /root/zynq/stuck-guard.img
set -uo pipefail
B=${B:-/root/zynq/plcheck-test/B.img}
S=${S:-/root/zynq/stuck-guard.img}
[ -f "$B" ] || { echo "no $B: run qemu_plcheck_test.sh first"; exit 1; }
cp "$B" "$S" || exit 1
M=$(mktemp -d)
start=$(sfdisk -J "$S" | python3 -c "import json,sys; print(json.load(sys.stdin)['partitiontable']['partitions'][1]['start'] * 512)")
mount -o loop,offset="$start" "$S" "$M" || { echo "cannot mount the root partition"; exit 1; }
G=$M/usr/local/sbin/zynq-plcheck
cp "$G" "$M.orig"
# the child that reads the PL: sleep (11 days) before mmap, so the guard waits on it exactly as on a stuck bus
sed -i 's/^PEEK = ("import mmap,os,struct,sys\\n"/PEEK = ("import mmap,os,struct,sys,time; time.sleep(1e6)\\n"/' "$G"
d=$(diff "$M.orig" "$G")
umount "$M"; rmdir "$M"; rm -f "$M.orig"
echo "$d"
[ "$(echo "$d" | grep -c '^[<>]')" = 2 ] && echo "$d" | grep -q 'time.sleep(1e6)' \
  && { echo "STUCK GUARD IMAGE: $S (one line changed: the PEEK child sleeps before reading)"; exit 0; }
echo "STUCK GUARD IMAGE: the patch did not apply as expected"; rm -f "$S"; exit 1
