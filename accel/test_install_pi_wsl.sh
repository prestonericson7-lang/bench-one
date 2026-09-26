#!/bin/bash
# test_install_pi_wsl.sh -- run the real Pi installer from the real bundle under systemd (WSL, root), with
# the network step off, and prove what it leaves behind; then undo it. The aarch64 binaries cannot run
# here -- this proves the install, not the programs (those have their own suites).
set -u
B=/mnt/d/espicpc/accel/accel-pi-bundle.tar.gz
W=/tmp/accel-bundle; rm -rf "$W"; mkdir -p "$W"
tar -xzf "$B" -C "$W" || { echo "FAIL: cannot unpack $B"; exit 1; }
had_nbdconf=0; [ -e /etc/modules-load.d/nbd.conf ] && had_nbdconf=1      # undo restores exactly this
ACCEL_NO_NET=1 bash "$W/install_pi.sh" > /tmp/accel-install.log 2>&1; rc=$?
tail -n 6 /tmp/accel-install.log | sed 's/^/  | /'
fail=0
chk() { if eval "$2"; then echo "  PASS  $1"; else echo "  FAIL  $1"; fail=1; fi; }
chk "installer exit 0"                          "[ $rc = 0 ]"
chk "zaccel-bench installed (aarch64, static)"  "file /usr/local/bin/zaccel-bench | grep -q 'aarch64.*statically'"
chk "libzaccel .a + .h"                         "[ -f /usr/local/lib/libzaccel.a ] && [ -f /usr/local/include/libzaccel.h ]"
chk "zaccel.py runs (python)"                   "python3 /usr/local/bin/zaccel.py --help >/dev/null 2>&1 || python3 -c 'import sys; sys.path.insert(0,\"/usr/local/lib/zaccel\"); import zaccel'"
chk "run_model + ppl installed (aarch64)"       "file /usr/local/bin/run_model /usr/local/bin/ppl | grep -c aarch64 | grep -q 2"
chk "GPU tools installed"                       "[ -x /usr/local/bin/gpu_selftest ] && [ -x /usr/local/bin/gpu_view ]"
chk "Teensy firmware + flasher"                 "ls /opt/accel/teensy/*.hex >/dev/null && [ -x /usr/local/bin/flash-teensy-gpu ]"
chk "bench-day installed"                       "[ -x /usr/local/bin/bench-day ]"
chk "zaccel-swap service enabled"               "systemctl is-enabled zaccel-swap.service >/dev/null"
chk "zaccel-swap retry timer present"           "[ -f /etc/systemd/system/zaccel-swap-retry.timer ]"
chk "nbd module configured to load at boot"     "grep -q nbd /etc/modules-load.d/nbd.conf"
chk "network untouched (no 10.77.0.1 here)"     "! ip -4 addr | grep -q 10.77.0.1"
echo "--- undo"
systemctl disable --now zaccel-swap.service zaccel-swap-retry.timer >/dev/null 2>&1
rm -f /etc/systemd/system/zaccel-swap*.service /etc/systemd/system/zaccel-swap-retry.timer /usr/local/sbin/zaccel-swap
rm -f /usr/local/bin/{zaccel-bench,zaccel.py,run_model,ppl,flash-teensy-gpu,bench-day} /usr/local/lib/libzaccel.a /usr/local/include/libzaccel.h
rm -rf /usr/local/lib/zaccel /opt/accel
for t in gpu_selftest gpu_demo gpu_view gpu_stat gpu_snap gpu_image; do rm -f /usr/local/bin/$t; done
rm -f /etc/udev/rules.d/49-fpgagpu-teensy.rules
[ $had_nbdconf = 0 ] && rm -f /etc/modules-load.d/nbd.conf
systemctl daemon-reload
[ $fail = 0 ] && echo "PI INSTALL TEST: PASS" || echo "PI INSTALL TEST: FAIL"
exit $fail
