#!/bin/bash
# test_pi_nbd_load.sh -- boot the Orange Pi's OWN kernel (vmlinux-6.6.98-sun60iw2 from the official image)
# under qemu-system-aarch64 and load our nbd.ko into it. Proves the module is accepted by the exact
# kernel the Pi runs (vermagic, every symbol, init) -- not just by symbol-table comparison.
# The vendor kernel has no serial driver QEMU can drive, so the verdict is the machine's exit:
#   /init_off  powers off at once            -> the kernel boots and runs our init
#   /init      finit_module(nbd.ko), then powers off only if /sys/block/nbd0 exists; else hangs
# Run inside WSL as root.
set -u
REPO=/mnt/d/espicpc
IMG=${IMG:-/mnt/c/Users/Danie/Downloads/opi4pro/Orangepi4pro_1.1.0_ubuntu_resolute_desktop_xfce_linux6.6.98.img}
KREL=6.6.98-sun60iw2
W=/root/opi/nbdload; rm -rf "$W"; mkdir -p "$W/root/sys" "$W/root/dev" "$W/root/proc"
M=$(mktemp -d); mount -o ro,loop,offset=$((32*1024*1024)) "$IMG" "$M" || exit 2
cp "$M/boot/vmlinux-$KREL" "$W/Image"; umount "$M"; rmdir "$M"
cp "$REPO/accel/mem/pi-kmod/$KREL/nbd.ko" "$W/root/nbd.ko"
cat > "$W/init.c" <<'EOF'
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static void off(void) { sync(); reboot(RB_POWER_OFF); for (;;) pause(); }
int main(int argc, char **argv) {
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    if (argv[0][5] == '_') off();                      /* /init_off */
    int fd = open("/nbd.ko", O_RDONLY);
    long r = syscall(SYS_finit_module, fd, "nbds_max=2", 0);
    struct stat st;
    if (fd >= 0 && r == 0 && stat("/sys/block/nbd0", &st) == 0 && stat("/sys/block/nbd1", &st) == 0) off();
    for (;;) pause();                                  /* failure: hang until the timeout */
}
EOF
aarch64-linux-gnu-gcc -static -O2 -o "$W/root/init" "$W/init.c" || exit 2
ln -f "$W/root/init" "$W/root/init_off"
(cd "$W/root" && find . | cpio -o -H newc 2>/dev/null | gzip -9) > "$W/initramfs.gz"
run() {
  timeout 180 qemu-system-aarch64 -M virt,gic-version=3 -cpu max -smp 2 -m 1024 -nographic -no-reboot -nic none \
    -kernel "$W/Image" -initrd "$W/initramfs.gz" -append "rdinit=$1 panic=-1" >"$W/$2.log" 2>&1
}
rc=0
run /init_off boot; b=$?
[ $b = 0 ] && echo "PASS  the official Pi kernel boots under QEMU and runs our init (exit $b)" \
           || { echo "FAIL  the official Pi kernel did not reach init under QEMU (exit $b) -- load test inconclusive"; exit 2; }
run /init load; l=$?
[ $l = 0 ] && echo "PASS  nbd.ko loaded into the official Pi kernel: /sys/block/nbd0 and nbd1 exist" \
           || { echo "FAIL  nbd.ko did not load into the official Pi kernel (exit $l)"; rc=1; }
exit $rc
