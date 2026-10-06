#!/bin/bash
# validate_jbd2_method.sh T... -- is "the last ext4 commit on the card bounds when the board stopped" true?
# Boots the card image in QEMU (a copy), cuts the power (SIGKILL) when the console's kernel clock passes T
# seconds, then reads the dead image exactly as card #1 was read: the boot's wall-clock start from its own
# journal, then jbd2_timeline.py on the unreplayed root partition. The method holds if, for every T during
# boot, the last commit lies within ~5 s (the ext4 commit interval) before T. WSL, root.
#   bash /mnt/d/espicpc/machine/cards/validate_jbd2_method.sh 15 30 45
set -u
REPO=/mnt/d/espicpc; OUT=$REPO/hardware/pz7020-starlite/linux/out
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
W=/root/zynq/jbd2-validate; mkdir -p "$W"
PORT=5571
for T in "$@"; do
  echo "== power cut at uptime $T s"
  cp --sparse=always "$IMG" "$W/sd.img" && truncate -s 8G "$W/sd.img"
  qemu-system-arm -M xilinx-zynq-a9 -m 512M -display none -monitor none \
    -serial tcp:127.0.0.1:$PORT,server=on,wait=on -kernel "$OUT/zImage" -dtb "$OUT/zynq-pz7020-starlite.dtb" \
    -append "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 cpufreq.off=1 uio_pdrv_genirq.of_id=generic-uio" \
    -drive file="$W/sd.img",if=sd,format=raw </dev/null > "$W/qemu.out" 2>&1 &
  Q=$!
  sleep 1
  python3 - "$PORT" "$T" "$Q" <<'PY'
import os, re, signal, socket, sys, time
port, T, pid = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
for _ in range(100):
    try:
        s = socket.create_connection(("127.0.0.1", port)); break
    except OSError:
        time.sleep(0.2)
s.settimeout(0.2); buf = b""; t0 = None; last = 0.0
# the cut is timed from the kernel's first line (uptime ~0) on this host's clock: after systemd starts, most
# console lines carry no kernel timestamp, so waiting for one would overshoot (the first version did)
while t0 is None or time.time() - t0 < T:
    try:
        c = s.recv(65536)
    except socket.timeout:
        c = b""
    if c:
        buf += c
        if t0 is None and b"Booting Linux" in buf:
            m = re.search(rb"\[ *([0-9]+\.[0-9]+)\] Booting Linux", buf)
            t0 = time.time() - (float(m.group(1)) if m else 0.0)
        stamps = re.findall(rb"\[ *([0-9]+\.[0-9]+)\]", buf[-4096:])
        if stamps:
            last = max(last, float(stamps[-1]))
os.kill(pid, signal.SIGKILL)
print(f"   power cut {time.time() - t0:.1f} s after the kernel started (last kernel timestamp seen: {last:.3f} s)")
PY
  wait $Q 2>/dev/null
  # the root partition as it was at the cut, and a replayed copy to read its journal from
  dd if="$W/sd.img" of="$W/p2.img" bs=1M skip=129 count=4096 conv=sparse status=none
  cp --sparse=always "$W/p2.img" "$W/p2r.img"; e2fsck -fy "$W/p2r.img" > /dev/null 2>&1
  mkdir -p "$W/mnt"; mount -o ro,loop,noload "$W/p2r.img" "$W/mnt"
  J=$(ls "$W"/mnt/var/log/journal/*/system.journal 2>/dev/null | head -1)
  if [ -z "$J" ]; then
    echo "   no persistent journal yet (cut before the flush): no wall-clock offset, nothing to compare"
  else
    last=$(chroot "$W/mnt" /bin/journalctl --file "${J#$W/mnt}" -o export 2>/dev/null | grep -a -E "^__(REALTIME|MONOTONIC)_TIMESTAMP=" \
        | paste - - | tail -1 | sed -E 's/__REALTIME_TIMESTAMP=([0-9]+)\t__MONOTONIC_TIMESTAMP=([0-9]+)/\1 \2/')
    if [ -z "$last" ]; then
      echo "   the journal file on the card has no entries yet: no wall-clock offset, nothing to compare"
      umount "$W/mnt"; continue
    fi
    B=$(echo "$last" | awk '{printf "%.3f", ($1 - $2) / 1e6}')
    echo "   boot's wall-clock start from its own journal: $B"
    # journald's bring-up setting (SyncIntervalSec=2s) should keep the card's journal within ~2 s of the cut
    echo "   last journal entry on the card at $(echo "$last" | awk '{printf "%.3f", $2 / 1e6}') s of uptime"
    python3 "$REPO/machine/cards/jbd2_timeline.py" "$W/p2.img" "$B" | grep -E "transaction\(s\)|== transaction" | sed 's/^/   /'
    python3 "$REPO/machine/cards/jbd2_timeline.py" "$W/p2.img" "$B" | grep -oE "committed at uptime +[0-9.]+" | grep -oE "[0-9.]+$" > "$W/commits.txt"
    lastc=$(tail -1 "$W/commits.txt")
    gap=$(awk 'NR > 1 { g = $1 - p; if (g > m) { m = g; a = p; b = $1 } } { p = $1 } END { printf "%.1f s (%.1f -> %.1f)", m, a, b }' "$W/commits.txt")
    echo "   RESULT  cut at $T s (PC clock), last commit on the card at ${lastc:-none} s, longest gap between commits $gap"
  fi
  umount "$W/mnt"
done
rm -f "$W"/*.img
