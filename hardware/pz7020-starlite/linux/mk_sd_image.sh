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
PL_BIT=${PL_BIT:-$REPO/hardware/pz7020-starlite/ps7-axi/build/pz7020_ps7_top.bit}   # first-boot PL: PS7 + register file (heartbeat, fan, time master)
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
[ -f "$OUT/modules.tar.gz" ] && tar xzf "$OUT/modules.tar.gz" -C "$ROOT/" && echo "modules: $(ls "$ROOT/lib/modules")"
mkdir -p "$ROOT/lib/firmware"
for b in "$REPO"/hardware/pz7020-starlite/fan/build/fan_top.bit "$REPO"/hardware/pz7020-starlite/ps7-axi/build/pz7020_ps7_top.bit "$REPO"/firmware/rtlsdr-pentest/fpga/openxc7/build/sdr_accel_zynq_top.bit; do
  [ -f "$b" ] && python3 "$REPO/hardware/pz7020-starlite/linux/bit2bin.py" "$b" "$ROOT/lib/firmware/$(basename "${b%.bit}").bin"
done
# --- the Zynq's telemetry agent (talks to the Pi hub) + its systemd unit ---
install -D -m 0755 "$REPO/firmware/telemetry-hub/zynq_agent.py" "$ROOT/usr/local/bin/zynq_agent.py"
install -D -m 0755 "$REPO/firmware/telemetry-hub/pl_regs.py" "$ROOT/usr/local/bin/pl_regs.py"
cat > "$ROOT/etc/systemd/system/zynq-agent.service" <<UNIT
[Unit]
Description=Zynq telemetry agent for the car hub (KEY=value lines on :8091)
After=network.target
[Service]
ExecStart=/usr/bin/python3 /usr/local/bin/zynq_agent.py
Restart=always
RestartSec=2
[Install]
WantedBy=multi-user.target
UNIT
mkdir -p "$ROOT/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/zynq-agent.service "$ROOT/etc/systemd/system/multi-user.target.wants/zynq-agent.service"
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
