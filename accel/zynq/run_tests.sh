#!/bin/bash
# Build zaccel-server and run test_server.py against it:
#   x86 --cpu, x86 --model, and the static armhf board binary under qemu-arm-static (--cpu, --model).
# Run in WSL Ubuntu-22.04:  bash /mnt/d/espicpc/accel/zynq/run_tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
bash build.sh || { echo "BUILD FAILED"; exit 1; }
QEMU="$(command -v qemu-arm-static || true)"
[ -n "$QEMU" ] || { echo "qemu-arm-static missing: apt-get install qemu-user-static"; exit 1; }

fail=0
run() { echo; python3 test_server.py "$@" || fail=1; }
run --bin out/zaccel-server-x86 --cpu
run --bin out/zaccel-server-x86 --model
run --bin out/zaccel-server-armhf --cpu --wrap "$QEMU"
run --bin out/zaccel-server-armhf --model --wrap "$QEMU"
echo
bash test_pl_detect.sh out/zaccel-server-x86 || fail=1
bash test_pl_detect.sh out/zaccel-server-armhf "$QEMU" || fail=1
echo
if [ $fail -eq 0 ]; then echo "RUN_TESTS: ALL CONFIGURATIONS PASSED"; else echo "RUN_TESTS: FAILURES"; fi
exit $fail
