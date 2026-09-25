#!/bin/bash
# qemu_serial_tcp.sh -- boot the SD image on QEMU's xilinx-zynq-a9 with the console (ttyPS0) on a TCP
#   socket instead of stdio, so watch_boot.ps1 -Tcp localhost:5555 can be tested against real Linux
#   (autologin, zynq-report.service, a typed zynq-report). QEMU waits for the watcher to connect.
#   Same limits as qemu_test.sh: no SPL/ps7_init, no DDR PHY, no PL -- the kernel is loaded directly.
#     wsl -d Ubuntu-22.04 -u root --exec bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_serial_tcp.sh
set -uo pipefail
REPO=${REPO:-/mnt/d/espicpc}
OUT=${OUT:-$REPO/hardware/pz7020-starlite/linux/out}
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
PORT=${PORT:-5555}
TIMEOUT=${TIMEOUT:-600}
cp "$IMG" /root/zynq/qemu-sd.img && truncate -s 2G /root/zynq/qemu-sd.img
echo "QEMU waiting for a console client on tcp port $PORT"
timeout "$TIMEOUT" qemu-system-arm -M xilinx-zynq-a9 -m 512M -display none -monitor none \
  -serial tcp:0.0.0.0:$PORT,server=on,wait=on \
  -kernel "$OUT/zImage" -dtb "$OUT/zynq-pz7020-starlite.dtb" \
  -append "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0" \
  -drive file=/root/zynq/qemu-sd.img,if=sd,format=raw </dev/null
echo "qemu exit $?"
