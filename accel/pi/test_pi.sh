#!/bin/bash
# test_pi.sh -- every accel/pi test, on a Linux x86-64 box (WSL Ubuntu here):
#   MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-22.04 -u root -- bash /mnt/d/espicpc/accel/pi/test_pi.sh
#
# Correctness only.  Nothing here times anything that counts: the PC, qemu and mock_server.py are
# not the Orange Pi and not the Zynq.  Logs: out/test/<name>.log
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
LOG="$OUT/test"
rm -rf "$LOG"
mkdir -p "$LOG"
QEMU="qemu-aarch64-static -cpu max"
PIDS=""
trap 'kill $PIDS 2>/dev/null' EXIT
PASSED=0
FAILED=()

# t NAME WANT_EXIT PATTERN CMD...   pass = exit code WANT_EXIT and PATTERN (egrep, "" = none) in the log
t() {
    local name="$1" want="$2" pat="$3"
    shift 3
    "$@" >"$LOG/$name.log" 2>&1
    local rc=$?
    local ok=1
    [ "$rc" -eq "$want" ] || ok=0
    if [ -n "$pat" ] && ! grep -Eq "$pat" "$LOG/$name.log"; then ok=0; fi
    if [ $ok -eq 1 ]; then
        PASSED=$((PASSED + 1)); echo "PASS  $name"
    else
        FAILED+=("$name (exit $rc, wanted $want${pat:+, pattern '$pat'})"); echo "FAIL  $name  (exit $rc, wanted $want)"
    fi
    tail -n "${TAILN:-3}" "$LOG/$name.log" | sed 's/^/        | /'
}

# mock NAME ARGS...  -> prints the port; the server's pid joins PIDS
mock() {
    local name="$1"; shift
    local pf="$LOG/mock-$name.port"
    python3 "$HERE/mock_server.py" --port 0 --port-file "$pf" "$@" >"$LOG/mock-$name.log" 2>&1 &
    PIDS="$PIDS $!"
    for _ in $(seq 100); do [ -s "$pf" ] && break; sleep 0.1; done
    cat "$pf"
}

is_static() { ! aarch64-linux-gnu-readelf -l "$1" | grep -q 'program interpreter'; }

cd "$HERE"
echo "== build"
TAILN=4 t build 0 'SDOT instructions' bash "$HERE/build.sh"

echo "== unit: packing, reference, CPU kernel (x86-64 portable path)"
t unit-x86        0 ' 0 failed' "$OUT/host/test_lib" --unit
t unit-x86-asan   0 ' 0 failed' "$OUT/host/test_lib_asan" --unit
t malformed-x86   0 ' 0 failed' "$OUT/host/test_lib" --malformed
t malformed-asan  0 ' 0 failed' "$OUT/host/test_lib_asan" --malformed

P=$(mock main)
PNOMEM=$(mock nomem --mem-mb 1)
PBAD=$(mock corrupt --corrupt-every 3)
echo "== mock servers on ports $P (main), $PNOMEM (1 MB), $PBAD (corrupts every 3rd GEMV)"

echo "== protocol, x86-64 client against the mock"
t server-x86      0 ' 0 failed' "$OUT/host/test_lib" --server 127.0.0.1 "$P"
t server-asan     0 ' 0 failed' "$OUT/host/test_lib_asan" --server 127.0.0.1 "$P"
t nomem-x86       0 ' 0 failed' "$OUT/host/test_lib" --nomem 127.0.0.1 "$PNOMEM"
t python-client   0 ' 0 failed' python3 "$HERE/test_py.py" 127.0.0.1 "$P"

echo "== bench, x86-64, against the mock (correctness only)"
SHAPES="-s 64x100:int4:1 -s 96x4096:int4:8 -s 128x333:int8:3 -s 50x2048:int8:8 -s 1x16:int4:2"
TAILN=6 t bench-x86 0 'every answer bit-exact; timings DO NOT COUNT' \
    "$OUT/host/zaccel-bench" -H 127.0.0.1 -p "$P" -r 3 $SHAPES
t bench-x86-catches-wrong 1 'RESULT: FAIL' \
    "$OUT/host/zaccel-bench" -H 127.0.0.1 -p "$PBAD" -r 3 -s 40x77:int4:4
t bench-x86-names-the-row 0 '' grep -Eq 'MISMATCH +Zynq +row [0-9]+ vec [0-9]+' "$LOG/bench-x86-catches-wrong.log"
t bench-x86-zynq-absent 2 'NOT REACHABLE' "$OUT/host/zaccel-bench" -H 127.0.0.1 -p 1 -r 2 -s 32x64:int8:2
TAILN=8 t bench-x86-default-shapes-pi-only 0 '2048x2048 int8 nb=8' "$OUT/host/zaccel-bench" -n -r 2

echo "== aarch64 static binaries under $QEMU"
t aarch64-static  0 '' is_static "$OUT/aarch64/zaccel-bench"
t aarch64-static2 0 '' is_static "$OUT/aarch64/test_lib"
t unit-aarch64      0 'neon-sdot' $QEMU "$OUT/aarch64/test_lib" --unit
t unit-aarch64-pass 0 '' grep -q ' 0 failed' "$LOG/unit-aarch64.log"
t malformed-aarch64 0 ' 0 failed' $QEMU "$OUT/aarch64/test_lib" --malformed
t server-aarch64    0 ' 0 failed' $QEMU "$OUT/aarch64/test_lib" --server 127.0.0.1 "$P"
t nomem-aarch64     0 ' 0 failed' $QEMU "$OUT/aarch64/test_lib" --nomem 127.0.0.1 "$PNOMEM"
TAILN=6 t bench-aarch64 0 'every answer bit-exact; timings DO NOT COUNT' \
    $QEMU "$OUT/aarch64/zaccel-bench" -H 127.0.0.1 -p "$P" -r 3 $SHAPES
t bench-aarch64-used-sdot 0 '' grep -q 'kernel neon-sdot' "$LOG/bench-aarch64.log"
TAILN=6 t bench-aarch64-1thread 0 'every answer bit-exact; timings DO NOT COUNT' \
    $QEMU "$OUT/aarch64/zaccel-bench" -H 127.0.0.1 -p "$P" -r 2 -t 1 -s 200x1000:int4:8 -s 300x999:int8:5
t bench-aarch64-catches-wrong 1 'RESULT: FAIL' \
    $QEMU "$OUT/aarch64/zaccel-bench" -H 127.0.0.1 -p "$PBAD" -r 3 -s 40x77:int8:4
TAILN=8 t bench-aarch64-default-shapes-pi-only 0 '2048x2048 int8 nb=8' \
    $QEMU "$OUT/aarch64/zaccel-bench" -n -r 1

echo
echo "== $PASSED passed, ${#FAILED[@]} failed"
for f in "${FAILED[@]}"; do echo "   FAILED: $f"; done
[ ${#FAILED[@]} -eq 0 ]
