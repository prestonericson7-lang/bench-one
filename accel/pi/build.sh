#!/bin/bash
# build.sh -- builds accel/pi into accel/pi/out/
#
#   out/host/     native build.  On an x86-64 PC these binaries exist for the TESTS ONLY (their
#                 timings mean nothing).  Run on the Orange Pi itself, this is a native aarch64
#                 build with -march=armv8.2-a+dotprod.
#   out/aarch64/  static aarch64 for the Orange Pi 4 Pro:
#                 aarch64-linux-gnu-gcc -O3 -march=armv8.2-a+dotprod -static
#                 Copy out/aarch64/zaccel-bench and zaccel.py to the Pi; nothing else is needed.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
cd "$HERE"
mkdir -p "$OUT/host" "$OUT/aarch64"

WARN="-std=c11 -Wall -Wextra -Wshadow -Wpointer-arith -Werror"
LIBSRC="libzaccel.c zaccel_cpu.c"

build_set() {   # build_set CC FLAGS DIR
    local cc="$1" flags="$2" dir="$3" f
    $cc $WARN $flags -pthread -o "$dir/zaccel-bench" zaccel-bench.c $LIBSRC
    $cc $WARN $flags -pthread -o "$dir/test_lib"     test_lib.c     $LIBSRC
    for f in libzaccel zaccel_cpu; do $cc $WARN $flags -c -o "$dir/$f.o" "$f.c"; done
    rm -f "$dir/libzaccel.a"
    ${AR:-ar} rcs "$dir/libzaccel.a" "$dir/libzaccel.o" "$dir/zaccel_cpu.o"
    rm -f "$dir/libzaccel.o" "$dir/zaccel_cpu.o"
}

ARCH="$(uname -m)"
CC="${CC:-gcc}"
HOSTFLAGS="-O3"
[ "$ARCH" = aarch64 ] && HOSTFLAGS="-O3 -march=armv8.2-a+dotprod"
echo "== host build ($ARCH): $CC $HOSTFLAGS"
build_set "$CC" "$HOSTFLAGS" "$OUT/host"
if [ "$ARCH" = x86_64 ]; then
    echo "== host test build with AddressSanitizer + UBSan"
    $CC $WARN -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
        -pthread -o "$OUT/host/test_lib_asan" test_lib.c $LIBSRC
fi

CROSS="${CROSS:-aarch64-linux-gnu-gcc}"
if command -v "$CROSS" >/dev/null 2>&1; then
    AFLAGS="-O3 -march=armv8.2-a+dotprod -static -DZACCEL_NO_RESOLVER"
    echo "== aarch64 static build: $CROSS $AFLAGS"
    AR="${CROSS%gcc}ar" build_set "$CROSS" "$AFLAGS" "$OUT/aarch64"
    OBJDUMP="${CROSS%gcc}objdump"
    if command -v "$OBJDUMP" >/dev/null 2>&1; then
        n=$("$OBJDUMP" -d "$OUT/aarch64/zaccel-bench" | grep -c $'\tsdot\t' || true)
        echo "   zaccel-bench contains $n SDOT instructions"
        [ "$n" -gt 0 ] || { echo "ERROR: no SDOT in the aarch64 build"; exit 1; }
    fi
else
    echo "== no $CROSS here: static aarch64 build skipped"
fi
ls -l "$OUT/host" "$OUT/aarch64"
