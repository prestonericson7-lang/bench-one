#!/bin/bash
# qemu_serial_tcp.sh -- boot the SD image on QEMU's xilinx-zynq-a9 with the console (ttyPS0) on a TCP
#   socket instead of stdio, so watch_boot.ps1 -Tcp localhost:5555 can be tested against real Linux
#   (autologin, zynq-report.service, a typed zynq-report). QEMU waits for the watcher to connect.
#   Same limits as qemu_test.sh: no SPL/ps7_init, no DDR PHY, no PL -- the kernel is loaded directly.
#     wsl -d Ubuntu-22.04 -u root --exec bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_serial_tcp.sh
set -uo pipefail
# QEMU wants an SD card whose size is a power of two: round the copy UP to one, never down (a fixed
# "truncate -s 2G" cut the 4.2 GB machine image inside its root filesystem -> "VFS: Unable to mount root")
sd_pow2() { local s p=1; s=$(stat -c %s "$1"); while [ "$p" -lt "$s" ]; do p=$((p * 2)); done; truncate -s "$p" "$1"; }
REPO=${REPO:-/mnt/d/espicpc}
OUT=${OUT:-$REPO/hardware/pz7020-starlite/linux/out}
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
PORT=${PORT:-5555}
TIMEOUT=${TIMEOUT:-600}
# EXTRA: more kernel arguments, e.g. EXTRA=fpgagpu.pl_loaded=1 to drive board_experiment.py's PL steps
EXTRA=${EXTRA:-}
# one copy per port: two instances must never share a disk image (the first version used one fixed file,
# and a second instance overwrote the image under a running one, 2026-10-06)
SD=/root/zynq/qemu-sd-$PORT.img
cp "$IMG" "$SD" && sd_pow2 "$SD"
echo "QEMU waiting for a console client on tcp port $PORT"
timeout "$TIMEOUT" qemu-system-arm -M xilinx-zynq-a9 -m 512M -display none -monitor none \
  -serial tcp:0.0.0.0:$PORT,server=on,wait=on \
  -kernel "$OUT/zImage" -dtb "$OUT/zynq-pz7020-starlite.dtb" \
  -append "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 $EXTRA" \
  -drive file="$SD",if=sd,format=raw </dev/null
echo "qemu exit $?"
