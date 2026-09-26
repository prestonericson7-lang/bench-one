#!/bin/bash
# Build zaccel-server (accel/SPEC.md sections 2-4):
#   out/zaccel-server-x86    native, for the tests
#   out/zaccel-server-armhf  static armhf, for the Zynq (installed as /usr/local/bin/zaccel-server)
# Run in WSL Ubuntu-22.04:  bash /mnt/d/espicpc/accel/zynq/build.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/zaccel-server.c"
OUT="$HERE/out"
mkdir -p "$OUT"
CFLAGS="-std=c11 -O2 -Wall -Wextra -Wshadow -pthread"

gcc $CFLAGS -o "$OUT/zaccel-server-x86" "$SRC"
arm-linux-gnueabihf-gcc $CFLAGS -static -o "$OUT/zaccel-server-armhf" "$SRC"

file "$OUT/zaccel-server-x86" "$OUT/zaccel-server-armhf"
(cd "$OUT" && sha256sum zaccel-server-x86 zaccel-server-armhf)
echo "BUILD OK"
