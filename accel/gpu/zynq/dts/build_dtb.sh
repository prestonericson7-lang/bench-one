#!/bin/bash
# zynq/dts/build_dtb.sh -- build the PZ7020-StarLite device tree with the FPGA-GPU reserved-memory
# node (SPEC 10) exactly the way the kernel build does (same cpp flags and include paths, the kernel
# tree's own scripts/dtc/dtc, same -W flags), then prove that the result differs from the deployed
# dtb only by that node. Run in WSL/Linux (as root when the kernel tree is under /root):
#     bash dts/build_dtb.sh            (or: make dtb)
# Inputs are only read: $LINUX (kernel 6.12 tree, default /root/zynq/linux) and $P1_IMG (the SD
# card's FAT partition image, default /root/zynq/sdbuild/p1.img).
# Output: $OUT (default zynq/build/dts)/zynq-pz7020-starlite-gpu.dtb + the decompiled diff.
# Checks (any failure -> exit 1):
#   1. the unmodified board dts rebuilt here is byte-identical to the kernel tree's dtb
#      (proves the toolchain and flags reproduce the original),
#   2. that dtb is byte-identical to the one on the SD card's FAT partition (the deployed one),
#   3. dtc -I dtb -O dts -s of original vs new: no line removed, and the added lines are exactly the
#      reserved-memory node with gpu@1e000000 { reg = <0x1e000000 0x2000000>; no-map; }.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
LINUX=${LINUX:-/root/zynq/linux}
P1_IMG=${P1_IMG:-/root/zynq/sdbuild/p1.img}
OUT=${OUT:-$HERE/../build/dts}
XDIR=$LINUX/arch/arm/boot/dts/xilinx
DTC=$LINUX/scripts/dtc/dtc
BOARD=zynq-pz7020-starlite
ORIG=$XDIR/$BOARD.dtb
SRC=$HERE/$BOARD-gpu.dts

die() { echo "build_dtb: FAIL: $*" >&2; exit 1; }
for f in "$XDIR/$BOARD.dts" "$XDIR/zynq-7000.dtsi" "$ORIG" "$SRC"; do
    [ -f "$f" ] || die "missing $f"
done
[ -x "$DTC" ] || die "missing $DTC (build the kernel tree's dtc first)"
mkdir -p "$OUT"

# identical to the kernel's cmd_dtc (see arch/arm/boot/dts/xilinx/.$BOARD.dtb.cmd)
build() {   # build SRC.dts OUT.dtb
    local src=$1 out=$2
    gcc -E -Wp,-MMD,"$out.d.pre.tmp" -nostdinc -I"$XDIR" -I"$LINUX/scripts/dtc/include-prefixes" \
        -undef -D__DTS__ -x assembler-with-cpp -o "$out.dts.tmp" "$src"
    "$DTC" -o "$out" -b 0 -i"$XDIR/" -i"$LINUX/scripts/dtc/include-prefixes" \
        -Wno-unique_unit_address -Wno-unit_address_vs_reg -Wno-avoid_unnecessary_addr_size \
        -Wno-alias_paths -Wno-graph_child_address -Wno-simple_bus_reg \
        -d "$out.d.dtc.tmp" "$out.dts.tmp"
    rm -f "$out.d.pre.tmp" "$out.d.dtc.tmp"
}

echo "== 1. rebuild the unmodified board dts with the kernel's flags"
build "$XDIR/$BOARD.dts" "$OUT/$BOARD-rebuilt.dtb"
cmp "$OUT/$BOARD-rebuilt.dtb" "$ORIG" || die "rebuilt $BOARD.dtb differs from $ORIG"
echo "   identical to $ORIG ($(stat -c %s "$ORIG") bytes)"

echo "== 2. the dtb on the SD card's FAT partition"
if [ -f "$P1_IMG" ] && command -v mcopy >/dev/null; then
    rm -f "$OUT/$BOARD-deployed.dtb"
    mcopy -n -i "$P1_IMG" "::/$BOARD.dtb" "$OUT/$BOARD-deployed.dtb"
    cmp "$OUT/$BOARD-deployed.dtb" "$ORIG" || die "the dtb in $P1_IMG differs from $ORIG"
    echo "   $P1_IMG ::/$BOARD.dtb is identical to $ORIG"
else
    echo "   (skipped: $P1_IMG or mcopy not available)"
fi

echo "== 3. build $BOARD-gpu.dts"
build "$SRC" "$OUT/$BOARD-gpu.dtb"
echo "   $OUT/$BOARD-gpu.dtb ($(stat -c %s "$OUT/$BOARD-gpu.dtb") bytes)"

echo "== 4. decompile both and diff"
"$DTC" -q -I dtb -O dts -s -o "$OUT/$BOARD-orig.decompiled.dts" "$ORIG"
"$DTC" -q -I dtb -O dts -s -o "$OUT/$BOARD-gpu.decompiled.dts" "$OUT/$BOARD-gpu.dtb"
diff -u "$OUT/$BOARD-orig.decompiled.dts" "$OUT/$BOARD-gpu.decompiled.dts" > "$OUT/dtb.diff" || true
cat "$OUT/dtb.diff"
removed=$(grep -c '^-[^-]' "$OUT/dtb.diff" || true)
[ "$removed" = 0 ] || die "$removed line(s) of the original dtb changed or removed"
# the added lines, whitespace-stripped, must be exactly the reserved-memory node (dtc -s sorts
# properties, so no-map comes before reg)
grep '^+[^+]' "$OUT/dtb.diff" | sed 's/^+//; s/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$' \
    > "$OUT/added.txt" || true
cat > "$OUT/expected.txt" <<'EOF'
reserved-memory {
#address-cells = <0x01>;
#size-cells = <0x01>;
ranges;
gpu@1e000000 {
no-map;
reg = <0x1e000000 0x2000000>;
};
};
EOF
cmp -s "$OUT/added.txt" "$OUT/expected.txt" || {
    echo "added lines:"; cat "$OUT/added.txt"
    die "the new dtb adds something other than the reserved-memory node"
}
rm -f "$OUT/added.txt" "$OUT/expected.txt" "$OUT"/*.dts.tmp
echo "build_dtb: OK -- only /reserved-memory/gpu@1e000000 (0x1e000000 + 32 MB, no-map) was added"
