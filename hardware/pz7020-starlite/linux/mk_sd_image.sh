#!/bin/bash
# mk_sd_image.sh -- assemble the bootable SD image for the PZ7020-StarLite WITHOUT loop devices or root
#   tricks: the FAT and ext4 partitions are built as files (mtools / mke2fs -d) and dd'd into place.
#     wsl -d Ubuntu-22.04 -u root -- bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/mk_sd_image.sh
#   Layout: p1 FAT32 128 MiB  boot.bin u-boot.img boot.scr zImage zynq-pz7020-starlite.dtb pl.bit
#           p2 ext4   rest    Debian bookworm armhf rootfs (build_rootfs.sh)
set -euo pipefail
REPO=${REPO:-/mnt/d/espicpc}
OUT=${OUT:-$REPO/hardware/pz7020-starlite/linux/out}
ROOT=${ROOT:-/root/zynq/rootfs}
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
DT=zynq-pz7020-starlite
PL_BIT=${PL_BIT:-$REPO/hardware/pz7020-starlite/vivado/build/system.bit}   # Vivado system design: PS7 + register file + GMII-to-RGMII (eth1)
BOOT_MB=128
ROOT_MB=${ROOT_MB:-1536}

for f in boot.bin u-boot.img boot.scr zImage $DT.dtb; do [ -f "$OUT/$f" ] || { echo "missing $OUT/$f"; exit 1; }; done
[ -d "$ROOT/etc" ] || { echo "missing rootfs $ROOT"; exit 1; }

W=/root/zynq/sdbuild; rm -rf "$W"; mkdir -p "$W"
# --- p1: FAT32 built with mtools ---
truncate -s ${BOOT_MB}M "$W/p1.img"
mkfs.vfat -F 32 -n BOOT "$W/p1.img" >/dev/null
for f in boot.bin u-boot.img boot.scr zImage $DT.dtb; do mcopy -i "$W/p1.img" "$OUT/$f" ::/; done
[ -f "$PL_BIT" ] && mcopy -i "$W/p1.img" "$PL_BIT" ::/pl.bit && echo "pl.bit = $PL_BIT"
mdir -i "$W/p1.img" ::/
# --- kernel modules into the rootfs (build_kernel.sh may have finished after build_rootfs.sh) ---
[ -f "$OUT/modules.tar.gz" ] && tar --keep-directory-symlink -xzf "$OUT/modules.tar.gz" -C "$ROOT/" && echo "modules: $(ls "$ROOT/lib/modules")"
mkdir -p "$ROOT/usr/lib/firmware"   # /lib is a symlink to usr/lib (merged-usr); never create a real /lib
for b in "$REPO"/hardware/pz7020-starlite/fan/build/fan_top.bit "$REPO"/hardware/pz7020-starlite/ps7-axi/build/pz7020_ps7_top.bit "$REPO"/firmware/rtlsdr-pentest/fpga/openxc7/build/sdr_accel_zynq_top.bit; do
  [ -f "$b" ] && python3 "$REPO/hardware/pz7020-starlite/linux/bit2bin.py" "$b" "$ROOT/usr/lib/firmware/$(basename "${b%.bit}").bin"
done
# --- the Zynq's telemetry agent (talks to the Pi hub) + its systemd unit ---
install -D -m 0755 "$REPO/firmware/telemetry-hub/zynq_agent.py" "$ROOT/usr/local/bin/zynq_agent.py"
install -D -m 0755 "$REPO/firmware/telemetry-hub/pl_regs.py" "$ROOT/usr/local/bin/pl_regs.py"
# An agent at /boot/overlay/zynq_agent.py (the FAT partition, writable from any PC) wins over the
# built-in one, so the agent can be updated by copying a file onto the card -- no image rewrite.
cat > "$ROOT/etc/systemd/system/zynq-agent.service" <<'UNIT'
[Unit]
Description=Zynq telemetry agent for the car hub (KEY=value lines on :8091, beacon on UDP 8092)
After=network.target systemd-networkd.service boot.mount
[Service]
ExecStart=/bin/sh -c 'if [ -f /boot/overlay/zynq_agent.py ]; then exec /usr/bin/python3 /boot/overlay/zynq_agent.py; else exec /usr/bin/python3 /usr/local/bin/zynq_agent.py; fi'
Restart=always
RestartSec=2
[Install]
WantedBy=multi-user.target
UNIT
# --- the accelerators the Orange Pi uses (accel/SPEC.md, accel/gpu/SPEC.md) ---
#   zaccel-server  TCP 8093: the PL matrix engine (CPU fallback when the PL is absent)
#   fpgagpud       TCP 7777: the PL GPU (HDMI out, Teensy geometry bus)
#   nbd-server     TCP 10809: export "zynqram", part of this board's DDR3 as the Pi's swap
A=$REPO/accel
ZACCEL_BIN=${ZACCEL_BIN:-$A/zynq/out/zaccel-server-armhf}
GPU_BIN=${GPU_BIN:-$A/gpu/zynq/build/fpgagpud}
for f in "$ZACCEL_BIN" "$A/zynq/zaccel-server.service" "$GPU_BIN" "$A/gpu/zynq/fpgagpud.service" "$A/mem/install_zynq.sh"; do
  [ -f "$f" ] || { echo "missing accelerator artifact $f"; exit 1; }
done
install -D -m 0755 "$ZACCEL_BIN" "$ROOT/usr/local/bin/zaccel-server"
install -D -m 0644 "$A/zynq/zaccel-server.service" "$ROOT/etc/systemd/system/zaccel-server.service"
install -D -m 0755 "$GPU_BIN" "$ROOT/usr/local/bin/fpgagpud"
install -D -m 0644 "$A/gpu/zynq/fpgagpud.service" "$ROOT/etc/systemd/system/fpgagpud.service"
sed -i 's/\r$//' "$ROOT/etc/systemd/system/zaccel-server.service" "$ROOT/etc/systemd/system/fpgagpud.service"
mkdir -p "$ROOT/etc/systemd/system/multi-user.target.wants"
for u in zaccel-server fpgagpud; do
  ln -sf /etc/systemd/system/$u.service "$ROOT/etc/systemd/system/multi-user.target.wants/$u.service"
done
bash "$A/mem/install_zynq.sh" "$ROOT"
[ -x "$ROOT/usr/bin/nbd-server" ] && echo "nbd-server: present" || { echo "*** nbd-server missing from the rootfs (build_rootfs.sh)"; exit 1; }
# --- networking: systemd-networkd instead of ifupdown ---
#   eth0 (PS PHY):  DHCP on the bench, the fixed car-LAN address 10.20.0.2/24 always, and IPv4
#                   link-local if nothing else answers -- the hub reaches it in every setting.
#   eth1 (PL PHY via the GMII-to-RGMII core): DHCP + link-local.
printf 'auto lo\niface lo inet loopback\n' > "$ROOT/etc/network/interfaces"
mkdir -p "$ROOT/etc/systemd/network"
#   10.77.0.2 is the FPGA-GPU's direct-cable address (accel/gpu/SPEC.md §10), kept alongside 10.20.0.2.
printf '[Match]\nName=eth0\n\n[Network]\nDHCP=ipv4\nLinkLocalAddressing=ipv4\nAddress=10.20.0.2/24\nAddress=10.77.0.2/24\n' \
  > "$ROOT/etc/systemd/network/20-eth0.network"
printf '[Match]\nName=eth1\n\n[Network]\nDHCP=ipv4\nLinkLocalAddressing=ipv4\n' \
  > "$ROOT/etc/systemd/network/21-eth1.network"
mkdir -p "$ROOT/etc/systemd/system/sockets.target.wants"
ln -sf /lib/systemd/system/systemd-networkd.service "$ROOT/etc/systemd/system/multi-user.target.wants/systemd-networkd.service"
ln -sf /lib/systemd/system/systemd-networkd.socket  "$ROOT/etc/systemd/system/sockets.target.wants/systemd-networkd.socket"
rm -f "$ROOT/etc/systemd/system/multi-user.target.wants/networking.service"
[ -f "$ROOT/lib/systemd/systemd-networkd" ] && echo "networkd: present" || echo "*** networkd binary missing from the rootfs"
mkdir -p "$ROOT/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/zynq-agent.service "$ROOT/etc/systemd/system/multi-user.target.wants/zynq-agent.service"
# --- hands-off bring-up: the board prints its own facts report on the console at every boot, and the
#     serial console logs in by itself, so a host watcher needs no typing (see watch_boot.ps1) ---
L=$REPO/hardware/pz7020-starlite/linux
install -D -m 0755 "$L/zynq-report" "$ROOT/usr/local/bin/zynq-report"
install -D -m 0644 "$L/zynq-report.service" "$ROOT/etc/systemd/system/zynq-report.service"
ln -sf /etc/systemd/system/zynq-report.service "$ROOT/etc/systemd/system/multi-user.target.wants/zynq-report.service"
install -D -m 0644 "$L/serial-autologin.conf" "$ROOT/etc/systemd/system/serial-getty@ttyPS0.service.d/autologin.conf"
sed -i 's/\r$//' "$ROOT/usr/local/bin/zynq-report" "$ROOT/etc/systemd/system/zynq-report.service" "$ROOT/etc/systemd/system/serial-getty@ttyPS0.service.d/autologin.conf"
# --- p2: ext4 populated from the rootfs tree (no mount needed) ---
truncate -s ${ROOT_MB}M "$W/p2.img"
mke2fs -q -t ext4 -L rootfs -d "$ROOT" "$W/p2.img"
# --- assemble: MBR, p1 at 1 MiB, p2 right after ---
truncate -s $((1 + BOOT_MB + ROOT_MB + 1))M "$IMG"
parted -s "$IMG" mklabel msdos \
  mkpart primary fat32 1MiB $((1 + BOOT_MB))MiB \
  mkpart primary ext4 $((1 + BOOT_MB))MiB $((1 + BOOT_MB + ROOT_MB))MiB \
  set 1 boot on
dd if="$W/p1.img" of="$IMG" bs=1M seek=1 conv=notrunc status=none
dd if="$W/p2.img" of="$IMG" bs=1M seek=$((1 + BOOT_MB)) conv=notrunc status=none
parted -s "$IMG" unit MiB print
sha256sum "$IMG" | tee "$OUT/sd-image.sha256"
# copy to the repo out dir compressed (the raw image is > 1.6 GB)
xz -T0 -3 -k -f "$IMG" && cp "$IMG.xz" "$OUT/" && ls -la "$OUT/$(basename "$IMG").xz"
echo "SD IMAGE DONE: $IMG (+ .xz in $OUT). Write with the unbuffered writer, then boot jumper = SD."
