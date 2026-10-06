#!/bin/bash
# validate_journal_sync.sh -- does the bring-up journald setting (SyncIntervalSec=2s,
# /etc/systemd/journald.conf.d/bringup.conf) really get the journal onto the card within ~2 s?
# A copy of the card image gets a tick service: a numbered line every 0.5 s, to the console and the
# journal. QEMU boots it; the power is cut (SIGKILL) T s after the first tick reaches the console; the dead
# image's journal is read. The gap between the last tick on the console and the last tick on the card is
# what a power cut would lose. Run twice: with the setting (as on card #1) and without it (the control).
#   bash /mnt/d/espicpc/machine/cards/validate_journal_sync.sh [T]          (WSL, root)
set -u
REPO=/mnt/d/espicpc; OUT=$REPO/hardware/pz7020-starlite/linux/out
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
T=${1:-20}
W=/root/zynq/journal-sync; mkdir -p "$W"
PORT=5572
for mode in with without; do
  echo "== $mode the 2 s setting: power cut $T s after the first tick"
  cp --sparse=always "$IMG" "$W/sd.img" && truncate -s 8G "$W/sd.img"
  L=$(losetup -f --show -o $((129 * 1048576)) --sizelimit $((4096 * 1048576)) "$W/sd.img")
  mkdir -p "$W/m" && mount "$L" "$W/m"
  cat > "$W/m/etc/systemd/system/tick.service" <<'EOF'
[Unit]
Description=test ticks: a numbered line every 0.5 s to the console and the journal
[Service]
ExecStart=/bin/sh -c 'i=0; while true; do i=$((i+1)); u=$(cut -d" " -f1 /proc/uptime); echo "TICK $i $u" > /dev/console; echo "TICK $i $u" | systemd-cat -t tick; sleep 0.5; done'
[Install]
WantedBy=multi-user.target
EOF
  ln -sf /etc/systemd/system/tick.service "$W/m/etc/systemd/system/multi-user.target.wants/tick.service"
  [ $mode = without ] && rm -f "$W/m/etc/systemd/journald.conf.d/bringup.conf"
  echo "   journald drop-in on this copy: $(ls "$W/m/etc/systemd/journald.conf.d/" 2>/dev/null | tr '\n' ' ')"
  umount "$W/m"; losetup -d "$L"
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
s.settimeout(0.2); buf = b""; t0 = None
while t0 is None or time.time() - t0 < T:
    try:
        c = s.recv(65536)
    except socket.timeout:
        c = b""
    buf += c
    if t0 is None and b"TICK " in buf:
        t0 = time.time()
os.kill(pid, signal.SIGKILL)
ticks = re.findall(rb"TICK (\d+) ([0-9.]+)", buf)
n, u = ticks[-1]
print(f"   power cut: the last tick on the console was #{int(n)} at uptime {float(u):.2f} s")
open("/tmp/console_last_tick", "w").write(f"{int(n)} {float(u)}")
PY
  wait $Q 2>/dev/null
  dd if="$W/sd.img" of="$W/p2.img" bs=1M skip=129 count=4096 conv=sparse status=none
  e2fsck -fy "$W/p2.img" > /dev/null 2>&1
  mkdir -p "$W/m"; mount -o ro,loop "$W/p2.img" "$W/m"
  J=$(ls "$W"/m/var/log/journal/*/system.journal 2>/dev/null | head -1)
  last=$(chroot "$W/m" /bin/journalctl --file "${J#$W/m}" -t tick -o cat 2>/dev/null | tail -1)
  echo "   the last tick on the card: ${last:-none}"
  read cn cu < /tmp/console_last_tick
  if [ -n "$last" ]; then
    set -- $last; echo "   RESULT $mode: the card is $((cn - $2)) ticks behind the console = $(echo "$cu $3" | awk '{printf "%.1f", $1 - $2}') s lost to the power cut"
  else
    echo "   RESULT $mode: no tick reached the card at all (the console reached #$cn at $cu s)"
  fi
  umount "$W/m"
done
rm -f "$W"/*.img
