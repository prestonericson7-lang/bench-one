#!/bin/bash
# install_pi.sh -- put everything the Orange Pi needs to use its accelerators in place. Run once on the
# Pi, as root, from the unpacked accel bundle (make_pi_bundle.sh):
#     sudo bash install_pi.sh
#   * matrix engine client: zaccel-bench, libzaccel (.a + .h), zaccel.py          (accel/pi)
#   * FPGA-GPU tools + the wired-port address + Teensy tty access (pi_setup.sh)  (accel/gpu/pi)
#   * the Teensy geometry-engine firmware, and teensy_loader_cli to flash it       (accel/gpu/teensy)
#   * the Zynq's DDR3 as swap: nbd module, nbd-client, zaccel-swap service         (accel/mem)
# Idempotent. Every step reports; the summary at the end says what is and is not reachable yet.
set -uo pipefail
A=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "run as root: sudo bash $0"; exit 1; }
fail=0
say() { printf '\n== %s\n' "$*"; }

say "packages"
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -q teensy-loader-cli nbd-client python3 >/tmp/accel-apt.log 2>&1 \
  && echo "teensy-loader-cli, nbd-client, python3 installed" \
  || echo "apt-get failed (offline?) -- /tmp/accel-apt.log; continuing with what is present"

say "matrix engine client (libzaccel, zaccel-bench, zaccel.py)"
install -D -m 0755 "$A/pi/out/aarch64/zaccel-bench" /usr/local/bin/zaccel-bench || fail=1
install -D -m 0644 "$A/pi/out/aarch64/libzaccel.a" /usr/local/lib/libzaccel.a || fail=1
install -D -m 0644 "$A/pi/libzaccel.h" /usr/local/include/libzaccel.h || fail=1
install -D -m 0755 "$A/pi/zaccel.py" /usr/local/lib/zaccel/zaccel.py || fail=1
ln -sf /usr/local/lib/zaccel/zaccel.py /usr/local/bin/zaccel.py
sed -i 's/\r$//' /usr/local/lib/zaccel/zaccel.py
echo "installed: /usr/local/bin/zaccel-bench, /usr/local/lib/libzaccel.a, /usr/local/include/libzaccel.h, /usr/local/bin/zaccel.py"

say "FPGA-GPU tools"
bash "$A/gpu/pi/pi_setup.sh" || fail=1

say "Teensy geometry engine firmware"
install -d /opt/accel/teensy
cp -f "$A"/gpu/teensy/out/600/teensy_gpu.ino.hex /opt/accel/teensy/ 2>/dev/null && ls -la /opt/accel/teensy/ || { echo "no Teensy .hex in the bundle"; fail=1; }
cat > /usr/local/bin/flash-teensy-gpu <<'EOF'
#!/bin/bash
# flash the geometry-engine firmware onto the Teensy 4.1 plugged into this Pi (press its button if asked)
set -e
hex=$(ls /opt/accel/teensy/*.hex | head -1)
echo "flashing $hex"
exec teensy_loader_cli --mcu=TEENSY41 -w -s -v "$hex"
EOF
chmod +x /usr/local/bin/flash-teensy-gpu
echo "installed: /usr/local/bin/flash-teensy-gpu"

say "the Zynq's DDR3 as swap"
bash "$A/mem/install_pi.sh" || fail=1

say "what is reachable now"
for h in 10.20.0.2 10.77.0.2; do
  ping -c1 -W1 "$h" >/dev/null 2>&1 && echo "Zynq answers at $h" || echo "no answer from $h (fine if the Zynq is off)"
done
timeout 10 python3 /usr/local/lib/zaccel/zaccel.py 2>&1 | head -5 || true
swapon --show 2>/dev/null || true
echo
[ $fail = 0 ] && echo "ACCEL INSTALLED." || { echo "ACCEL INSTALL FINISHED WITH ERRORS (see above)"; exit 3; }
echo "Next: flash-teensy-gpu (Teensy on USB), then zaccel-bench and gpu_selftest with the Zynq up."
