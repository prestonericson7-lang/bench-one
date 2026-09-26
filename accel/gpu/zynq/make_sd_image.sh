#!/bin/bash
# zynq/make_sd_image.sh -- FPGA-GPU SD card image for the PZ7020-StarLite (SPEC 10, 12).
#
# Run in WSL (Ubuntu 22.04) as root:    bash make_sd_image.sh      (or: make image)
#
# Builds $OUTDIR/pz7020-gpu-sd.img (+ .img.xz if xz exists) from COPIES of the board's existing
# image $BASE_IMG (read only, never modified), changing only:
#   FAT  zynq-pz7020-starlite.dtb  <- build/dts/zynq-pz7020-starlite-gpu.dtb (dts/build_dtb.sh:
#                                     board dtb + reserved-memory gpu@1e000000, 32 MB, no-map)
#        boot.scr                  <- boot/boot.cmd via U-Boot mkimage (fdt_high below the GPU
#                                     window; fpgagpu.pl_loaded=1 when fpga loadb succeeded)
#        boot.cmd                  <- its source, for reference (U-Boot ignores it)
#        pl.bit                    <- $PROJ/out/pl.bit if it exists, else the old one is KEPT (warning)
#   ext4 /usr/local/bin/fpgagpud                        build/fpgagpud (static ARMv7, 0755)
#        /etc/systemd/system/fpgagpud.service           rootfs/... (0644)
#        /etc/systemd/system/multi-user.target.wants/fpgagpud.service (symlink, = systemctl enable)
#        /etc/default/fpgagpud                          rootfs/... (0644)
#        /etc/network/interfaces                        rootfs/... (0644): eth0 = DHCP in the
#                                                       background + always 10.77.0.2/24
#        /etc/network/interfaces.board-orig             the board's original file (for reverting)
#        /etc/fpga-gpu-image                            what was installed (checksums, date)
# Partitions are edited as files with mtools (FAT) and debugfs (ext4): no mounting is needed to
# build. Afterwards both partitions of the final image are loop-mounted READ-ONLY (if the kernel
# allows loop devices) and every installed file is compared with its source; e2fsck -fn and
# fsck.fat -n must pass; a manifest is printed and written next to the image.
# Nothing outside $OUTDIR is written (the mount point is $OUTDIR/mnt), no block device is touched.
set -euo pipefail
umask 022

HERE=$(cd "$(dirname "$0")" && pwd)
PROJ=$(cd "$HERE/.." && pwd)
ZYNQ=${ZYNQ:-/root/zynq}
BASE_IMG=${BASE_IMG:-$ZYNQ/pz7020-starlite-sd.img}
OUTDIR=${OUTDIR:-$ZYNQ/gpu-out}
MKIMAGE=${MKIMAGE:-$ZYNQ/u-boot/tools/mkimage}
IMG=$OUTDIR/pz7020-gpu-sd.img
WORK=$OUTDIR/work
MNT=$OUTDIR/mnt
DAEMON=$HERE/build/fpgagpud
DTB=$HERE/build/dts/zynq-pz7020-starlite-gpu.dtb
BOOTCMD=$HERE/boot/boot.cmd
RFS=$HERE/rootfs
PLBIT=$PROJ/out/pl.bit
DTB_NAME=zynq-pz7020-starlite.dtb

log()  { echo "[make_sd_image] $*"; }
warn() { echo "[make_sd_image] WARNING: $*" >&2; }
die()  { echo "[make_sd_image] ERROR: $*" >&2; exit 1; }

# ---- preconditions ------------------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "run as root (the base image and /root/zynq belong to root)"
for t in sfdisk mcopy mdir mdel debugfs e2fsck fsck.fat sha256sum dd cmp file; do
    command -v "$t" >/dev/null || die "missing tool: $t"
done
[ -x "$MKIMAGE" ] || die "missing U-Boot mkimage at $MKIMAGE"
[ -f "$BASE_IMG" ] || die "missing base image $BASE_IMG"
case "$OUTDIR" in /root/zynq/gpu-out|/root/zynq/gpu-out/*) ;; *)
    [ "${ALLOW_OUTDIR:-0}" = 1 ] || die "OUTDIR must be under /root/zynq/gpu-out (set ALLOW_OUTDIR=1 to override)";;
esac
for f in "$BOOTCMD" "$RFS/etc/network/interfaces" "$RFS/etc/systemd/system/fpgagpud.service" \
         "$RFS/etc/default/fpgagpud"; do
    [ -f "$f" ] || die "missing $f"
done

log "building the daemon (make arm) and the device tree (dts/build_dtb.sh, verified)"
make -s -C "$HERE" arm >/dev/null
bash "$HERE/dts/build_dtb.sh" > "$HERE/build/dts-build.log" 2>&1 || {
    cat "$HERE/build/dts-build.log"; die "device tree build/verification failed"; }
tail -1 "$HERE/build/dts-build.log"
case "$(file -b "$DAEMON")" in
    *"ARM, EABI5"*"statically linked"*) ;;
    *) die "$DAEMON is not a static ARM EABI5 binary";;
esac
[ -f "$DTB" ] || die "missing $DTB"

# ---- partition layout of the base image --------------------------------------------------------
mkdir -p "$OUTDIR" "$WORK" "$MNT"
for m in "$MNT/p1" "$MNT/p2"; do
    if mountpoint -q "$m"; then die "$m is still mounted (umount it first)"; fi
done
SFD=$(sfdisk -d "$BASE_IMG")
part() {   # part N -> "start size" in sectors
    echo "$SFD" | sed -n "s|^.*img$1 : start= *\([0-9]*\), size= *\([0-9]*\),.*|\1 \2|p"
}
read -r P1_START P1_SIZE <<<"$(part 1)"
read -r P2_START P2_SIZE <<<"$(part 2)"
[ -n "${P1_START:-}" ] && [ -n "${P2_START:-}" ] || die "cannot read the partition table of $BASE_IMG"
grep -q "img1 .*type=c" <<<"$SFD" || die "partition 1 of $BASE_IMG is not FAT32 (type c)"
grep -q "img2 .*type=83" <<<"$SFD" || die "partition 2 of $BASE_IMG is not Linux (type 83)"
log "base image $BASE_IMG: p1 FAT32 at sector $P1_START ($((P1_SIZE / 2048)) MiB), p2 ext4 at sector $P2_START ($((P2_SIZE / 2048)) MiB)"

# ---- copies -------------------------------------------------------------------------------------
rm -f "$IMG" "$IMG.tmp" "$IMG.xz" "$IMG.xz.tmp"
rm -rf "$WORK"
mkdir -p "$WORK"
log "copying the base image and extracting its partitions (sparse)"
cp --sparse=always "$BASE_IMG" "$IMG.tmp"
dd if="$BASE_IMG" of="$WORK/p1.img" bs=512 skip="$P1_START" count="$P1_SIZE" conv=sparse status=none
dd if="$BASE_IMG" of="$WORK/p2.img" bs=512 skip="$P2_START" count="$P2_SIZE" conv=sparse status=none
export MTOOLS_SKIP_CHECK=1
if mdir -i "$WORK/p1.img" ::/pl.bit >/dev/null 2>&1; then
    mcopy -n -i "$WORK/p1.img" ::/pl.bit "$WORK/pl.bit.base"
fi

# ---- FAT: dtb, boot.scr, pl.bit -----------------------------------------------------------------
"$MKIMAGE" -A arm -O linux -T script -C none -a 0 -e 0 -n "FPGA-GPU boot script" \
    -d "$BOOTCMD" "$WORK/boot.scr" >/dev/null
mcopy -o -i "$WORK/p1.img" "$DTB" "::/$DTB_NAME"
mcopy -o -i "$WORK/p1.img" "$WORK/boot.scr" ::/boot.scr
mcopy -o -i "$WORK/p1.img" "$BOOTCMD" ::/boot.cmd
PL_STATUS=
if [ -f "$PLBIT" ]; then
    # a Vivado .bit: 13-byte header 00 09 0f f0 0f f0 0f f0 0f f0 00 00 01, then key 'a' ...
    magic=$(head -c 13 "$PLBIT" | od -An -tx1 | tr -d ' \n')
    [ "$magic" = "00090ff00ff00ff00ff0000001" ] || die "$PLBIT is not a Vivado .bit file (header $magic)"
    grep -aq "7z020clg400" "$PLBIT" || die "$PLBIT is not for the xc7z020clg400"
    mcopy -o -i "$WORK/p1.img" "$PLBIT" ::/pl.bit
    PL_STATUS="pl.bit = $PLBIT ($(stat -c %s "$PLBIT") bytes, sha256 $(sha256sum "$PLBIT" | cut -c1-16)...)"
    log "$PL_STATUS"
else
    PL_STATUS="pl.bit KEPT from the base image (NOT the FPGA-GPU bitstream: $PLBIT does not exist)"
    warn "$PLBIT does not exist: the image keeps the board's OLD pl.bit. U-Boot loads it and"
    warn "fpgagpud will report 'a different bitstream is loaded' until out/pl.bit is built and"
    warn "this script is run again (or pl.bit is replaced on the card's FAT partition)."
fi

# ---- ext4: daemon, service, network -------------------------------------------------------------
dbg() { debugfs -w -R "$1" "$WORK/p2.img" 2>&1 | grep -v "^debugfs [0-9]" || true; }
# (grep -c reads all input: with pipefail, grep -q could SIGPIPE debugfs and report a false miss)
exists() { [ "$(debugfs -R "stat $1" "$WORK/p2.img" 2>/dev/null | grep -c "^Inode:")" -gt 0 ]; }
put() {    # put LOCAL DEST MODE
    local src=$1 dst=$2 mode=$3
    if exists "$dst"; then dbg "rm $dst" >/dev/null; fi
    dbg "write $src $dst" >/dev/null
    dbg "sif $dst mode 0100$mode" >/dev/null
    dbg "sif $dst uid 0" >/dev/null
    dbg "sif $dst gid 0" >/dev/null
    exists "$dst" || die "debugfs could not write $dst"
}
exists /etc/network/interfaces || die "the base rootfs has no /etc/network/interfaces (not ifupdown?)"
exists /etc/systemd/system/multi-user.target.wants || die "the base rootfs has no multi-user.target.wants"
exists /usr/local/bin || die "the base rootfs has no /usr/local/bin"

debugfs -R "cat /etc/network/interfaces" "$WORK/p2.img" 2>/dev/null > "$WORK/interfaces.board-orig"
[ -s "$WORK/interfaces.board-orig" ] || die "cannot read the base /etc/network/interfaces"
put "$DAEMON" /usr/local/bin/fpgagpud 755
put "$RFS/etc/systemd/system/fpgagpud.service" /etc/systemd/system/fpgagpud.service 644
put "$RFS/etc/default/fpgagpud" /etc/default/fpgagpud 644
put "$WORK/interfaces.board-orig" /etc/network/interfaces.board-orig 644
put "$RFS/etc/network/interfaces" /etc/network/interfaces 644
WANT=/etc/systemd/system/multi-user.target.wants/fpgagpud.service
if exists "$WANT"; then dbg "rm $WANT" >/dev/null; fi
dbg "symlink $WANT /etc/systemd/system/fpgagpud.service" >/dev/null   # as systemctl enable
exists "$WANT" || die "debugfs could not create $WANT"

{
    echo "FPGA-GPU SD image, built $(date -u '+%Y-%m-%d %H:%M:%S UTC') by zynq/make_sd_image.sh"
    echo "base image: $(basename "$BASE_IMG") ($(stat -c %s "$BASE_IMG") bytes)"
    echo "fpgagpud sha256 $(sha256sum "$DAEMON" | cut -d' ' -f1)"
    echo "dtb      sha256 $(sha256sum "$DTB" | cut -d' ' -f1) (board dtb + reserved-memory gpu@1e000000)"
    echo "boot.scr sha256 $(sha256sum "$WORK/boot.scr" | cut -d' ' -f1)"
    echo "$PL_STATUS"
} > "$WORK/fpga-gpu-image"
put "$WORK/fpga-gpu-image" /etc/fpga-gpu-image 644

log "checking both file systems"
e2fsck -fn "$WORK/p2.img" > "$WORK/e2fsck.log" 2>&1 || { cat "$WORK/e2fsck.log"; die "e2fsck -fn failed"; }
fsck.fat -n "$WORK/p1.img" > "$WORK/fsckfat.log" 2>&1 || { cat "$WORK/fsckfat.log"; die "fsck.fat -n failed"; }
log "  $(tail -1 "$WORK/e2fsck.log")"
log "  $(tail -1 "$WORK/fsckfat.log")"

# ---- assemble -----------------------------------------------------------------------------------
dd if="$WORK/p1.img" of="$IMG.tmp" bs=512 seek="$P1_START" conv=notrunc,sparse status=none
dd if="$WORK/p2.img" of="$IMG.tmp" bs=512 seek="$P2_START" conv=notrunc,sparse status=none
[ "$(stat -c %s "$IMG.tmp")" = "$(stat -c %s "$BASE_IMG")" ] || die "image size changed"
cmp -s <(sfdisk -d "$IMG.tmp" | grep -v '^device:' | sed 's|^[^ ]*\([0-9]\) :|p\1 :|') \
       <(echo "$SFD" | grep -v '^device:' | sed 's|^[^ ]*\([0-9]\) :|p\1 :|') || die "partition table changed"
cmp -s <(dd if="$IMG.tmp" bs=512 skip="$P1_START" count="$P1_SIZE" status=none) "$WORK/p1.img" || die "p1 copy-back mismatch"
cmp -s <(dd if="$IMG.tmp" bs=512 skip="$P2_START" count="$P2_SIZE" status=none) "$WORK/p2.img" || die "p2 copy-back mismatch"
mv "$IMG.tmp" "$IMG"
log "image written: $IMG"

# ---- verify: mount read-only (or read with mtools/debugfs) and compare every installed file ------
FAILS=0
chk() {    # chk NAME GOT_FILE WANT_FILE
    if cmp -s "$2" "$3"; then echo "  ok    $1"; else echo "  FAIL  $1"; FAILS=$((FAILS + 1)); fi
}
MOUNTED=0
cleanup() { if [ "$MOUNTED" = 1 ]; then umount "$MNT/p1" "$MNT/p2" 2>/dev/null || true; fi; }
trap cleanup EXIT
mkdir -p "$MNT/p1" "$MNT/p2"
if mount -o ro,loop,offset=$((P1_START * 512)),sizelimit=$((P1_SIZE * 512)) -t vfat "$IMG" "$MNT/p1" 2>/dev/null; then
    if mount -o ro,noload,loop,offset=$((P2_START * 512)),sizelimit=$((P2_SIZE * 512)) -t ext4 "$IMG" "$MNT/p2" 2>/dev/null; then
        MOUNTED=1
    else
        umount "$MNT/p1"
    fi
fi
VER=$WORK/verify
mkdir -p "$VER"
if [ "$MOUNTED" = 1 ]; then
    log "verifying: both partitions loop-mounted read-only at $MNT/p1 and $MNT/p2"
    F1() { cp "$MNT/p1/$1" "$VER/$2"; }
    F2() { cp "$MNT/p2$1" "$VER/$2"; }
    LINK=$(readlink "$MNT/p2$WANT")
    MODE_D=$(stat -c %a:%u:%g "$MNT/p2/usr/local/bin/fpgagpud")
    MODE_S=$(stat -c %a:%u:%g "$MNT/p2/etc/systemd/system/fpgagpud.service")
    MODE_N=$(stat -c %a:%u:%g "$MNT/p2/etc/network/interfaces")
    LISTING=$(ls -l "$MNT/p1")
else
    log "verifying: loop mount not available -- reading the final image with mtools/debugfs instead"
    dd if="$IMG" of="$VER/p1.img" bs=512 skip="$P1_START" count="$P1_SIZE" conv=sparse status=none
    dd if="$IMG" of="$VER/p2.img" bs=512 skip="$P2_START" count="$P2_SIZE" conv=sparse status=none
    F1() { mcopy -n -i "$VER/p1.img" "::/$1" "$VER/$2"; }
    F2() { debugfs -R "dump $1 $VER/$2" "$VER/p2.img" >/dev/null 2>&1; }
    LINK=$(debugfs -R "stat $WANT" "$VER/p2.img" 2>/dev/null | sed -n 's/^Fast link dest: "\(.*\)"/\1/p')
    st() { debugfs -R "stat $1" "$VER/p2.img" 2>/dev/null |
           awk '{for (i = 1; i < NF; i++) if ($i == "Mode:") m = $(i + 1)} /^User:/{u = $2; g = $4}
                END{sub(/^0+/, "", m); print m ":" u ":" g}'; }
    MODE_D=$(st /usr/local/bin/fpgagpud); MODE_S=$(st /etc/systemd/system/fpgagpud.service)
    MODE_N=$(st /etc/network/interfaces)
    LISTING=$(mdir -i "$VER/p1.img" ::/)
fi
F1 "$DTB_NAME" dtb; F1 boot.scr boot.scr; F1 boot.cmd boot.cmd
if [ -f "$PLBIT" ] || [ -f "$WORK/pl.bit.base" ]; then F1 pl.bit pl.bit; fi
F2 /usr/local/bin/fpgagpud fpgagpud; F2 /etc/systemd/system/fpgagpud.service service
F2 /etc/default/fpgagpud default; F2 /etc/network/interfaces interfaces
F2 /etc/network/interfaces.board-orig interfaces.orig; F2 /etc/fpga-gpu-image release
chk "FAT  /$DTB_NAME == build/dts/zynq-pz7020-starlite-gpu.dtb" "$VER/dtb" "$DTB"
chk "FAT  /boot.scr == mkimage(boot/boot.cmd)" "$VER/boot.scr" "$WORK/boot.scr"
chk "FAT  /boot.cmd == boot/boot.cmd" "$VER/boot.cmd" "$BOOTCMD"
if [ -f "$PLBIT" ]; then
    chk "FAT  /pl.bit == out/pl.bit" "$VER/pl.bit" "$PLBIT"
elif [ -f "$WORK/pl.bit.base" ]; then
    chk "FAT  /pl.bit unchanged from the base image (old design)" "$VER/pl.bit" "$WORK/pl.bit.base"
fi
chk "ext4 /usr/local/bin/fpgagpud == build/fpgagpud" "$VER/fpgagpud" "$DAEMON"
chk "ext4 /etc/systemd/system/fpgagpud.service" "$VER/service" "$RFS/etc/systemd/system/fpgagpud.service"
chk "ext4 /etc/default/fpgagpud" "$VER/default" "$RFS/etc/default/fpgagpud"
chk "ext4 /etc/network/interfaces" "$VER/interfaces" "$RFS/etc/network/interfaces"
chk "ext4 /etc/network/interfaces.board-orig == base image's interfaces" "$VER/interfaces.orig" "$WORK/interfaces.board-orig"
chk "ext4 /etc/fpga-gpu-image" "$VER/release" "$WORK/fpga-gpu-image"
if [ "$LINK" = "/etc/systemd/system/fpgagpud.service" ]; then echo "  ok    ext4 $WANT -> $LINK (service enabled)"
else echo "  FAIL  ext4 $WANT -> '$LINK'"; FAILS=$((FAILS + 1)); fi
for m in "fpgagpud:$MODE_D:755:0:0" "fpgagpud.service:$MODE_S:644:0:0" "interfaces:$MODE_N:644:0:0"; do
    IFS=: read -r n a u g wa wu wg <<<"$m"
    if [ "$a:$u:$g" = "$wa:$wu:$wg" ]; then echo "  ok    ext4 $n mode $a owner $u:$g"
    else echo "  FAIL  ext4 $n mode/owner $a:$u:$g (want $wa:$wu:$wg)"; FAILS=$((FAILS + 1)); fi
done
cleanup; MOUNTED=0
[ "$FAILS" = 0 ] || die "$FAILS verification check(s) failed"

# ---- compress + manifest ------------------------------------------------------------------------
XZ_LINE="(xz not installed: no .img.xz)"
if command -v xz >/dev/null; then
    log "compressing (xz -T0 -6)"
    xz -T0 -6 -c "$IMG" > "$IMG.xz.tmp"
    xz -t "$IMG.xz.tmp"
    mv "$IMG.xz.tmp" "$IMG.xz"
    XZ_LINE="$IMG.xz  $(stat -c %s "$IMG.xz") bytes  sha256 $(sha256sum "$IMG.xz" | cut -d' ' -f1)"
fi
{
    echo "==== FPGA-GPU SD image manifest ===="
    echo "image     $IMG  $(stat -c %s "$IMG") bytes  sha256 $(sha256sum "$IMG" | cut -d' ' -f1)"
    echo "xz        $XZ_LINE"
    echo "base      $BASE_IMG (unchanged; same partition table)"
    grep "img[0-9] :" <<<"$SFD" | sed "s|^.*\.img|  p|"
    echo "FAT (p1):"
    echo "$LISTING" | sed 's/^/  /'
    echo "  changed: $DTB_NAME (reserved-memory added), boot.scr (fdt_high + marker), boot.cmd (new)"
    echo "  $PL_STATUS"
    echo "ext4 (p2) changed/added:"
    for f in /usr/local/bin/fpgagpud /etc/systemd/system/fpgagpud.service "$WANT" /etc/default/fpgagpud \
             /etc/network/interfaces /etc/network/interfaces.board-orig /etc/fpga-gpu-image; do
        echo "  $f"
    done
    echo "checksums:"
    (cd "$VER" && sha256sum dtb boot.scr fpgagpud service interfaces release | sed 's/^/  /')
    echo "verification: all installed files compared with their sources: OK; e2fsck -fn and fsck.fat -n: clean"
    if [ -f "$PLBIT" ]; then echo "pl.bit: FPGA-GPU bitstream installed"; else
        echo "pl.bit: WARNING -- old board bitstream kept; build out/pl.bit and rerun"; fi
    echo "flash (on the Windows side, by hand): write the .img or .img.xz with a card imager"
} | tee "$OUTDIR/manifest.txt"
rm -rf "$VER" "$WORK"
rmdir "$MNT/p1" "$MNT/p2" "$MNT" 2>/dev/null || true
log "done"
