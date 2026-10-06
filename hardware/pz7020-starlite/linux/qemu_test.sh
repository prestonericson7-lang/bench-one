#!/bin/bash
# qemu_test.sh -- boot the kernel + DTB + SD image on QEMU's xilinx-zynq-a9 machine, inside WSL.
#   This is NOT the board: QEMU models the Zynq PS peripherals (uart0 at E0000000, sdhci0, gem0, ...)
#   but not the DDR PHY, so U-Boot SPL/ps7_init is skipped and the kernel is loaded directly.
#   What it proves: the kernel config, our device tree, the rootfs and the init/getty/ssh setup boot
#   to a login prompt on ttyPS0 with root on /dev/mmcblk0p2. What it cannot prove: ps7_init, the PHY
#   delay mode, the real SD controller timing, the PL.
#     wsl -d Ubuntu-22.04 -u root -- bash -c 'bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_test.sh'
set -uo pipefail
# QEMU wants an SD card whose size is a power of two: round the copy UP to one, never down (a fixed
# "truncate -s 2G" cut the 4.2 GB machine image inside its root filesystem -> "VFS: Unable to mount root")
sd_pow2() { local s p=1; s=$(stat -c %s "$1"); while [ "$p" -lt "$s" ]; do p=$((p * 2)); done; truncate -s "$p" "$1"; }
REPO=${REPO:-/mnt/d/espicpc}
OUT=${OUT:-$REPO/hardware/pz7020-starlite/linux/out}
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
DT=zynq-pz7020-starlite
LOG=/root/zynq/qemu-boot.log
TIMEOUT=${TIMEOUT:-180}

[ -f "$IMG" ] || { echo "no SD image at $IMG (run mk_sd_image.sh)"; exit 1; }
cp "$IMG" /root/zynq/qemu-sd.img            # QEMU writes to it; keep the master pristine
sd_pow2 /root/zynq/qemu-sd.img       # rounded up to a power of two (QEMU's rule)
MEM=${MEM:-512M}                            # the board: one x16 MT41K256M16 = 512 MB (16-bit DDR)
# cpufreq.off=1: QEMU clocks the A9 at 666 MHz while the DTB pins the board's real 766 MHz operating
# point; on the board the clock matches the table, so the board needs no such flag
timeout "$TIMEOUT" qemu-system-arm -M xilinx-zynq-a9 -m "$MEM" -nographic \
  -serial mon:stdio \
  -kernel "$OUT/zImage" -dtb "$OUT/$DT.dtb" \
  -append "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 cpufreq.off=1" \
  -drive file=/root/zynq/qemu-sd.img,if=sd,format=raw </dev/null >"$LOG" 2>&1
rc=$?
echo "qemu exit $rc (124 = timeout, expected: we never log in)"
echo "---- boot log excerpts ----"
grep -a -n -E "e000c000|eth1|gmii|uio|Booting Linux|Machine model|Memory:|mmc0|mmcblk0:|EXT4-fs \(mmcblk0p2\): mounted|systemd\[1\]|Welcome|login:|Kernel panic|VFS: Unable to mount|zynq-agent|ttyPS0" "$LOG" | head -30
if grep -a -q "login:" "$LOG"; then echo "QEMU BOOT: reached the login prompt"; exit 0; fi
if grep -a -q "Kernel panic" "$LOG"; then echo "QEMU BOOT: kernel panic"; tail -15 "$LOG"; exit 2; fi
echo "QEMU BOOT: no login prompt within ${TIMEOUT}s"; tail -15 "$LOG"; exit 3
