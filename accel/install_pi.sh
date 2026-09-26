#!/bin/bash
# install_pi.sh -- put everything the Orange Pi needs to use its accelerators in place. Run once on the
# Pi, as root, from the unpacked accel bundle (make_pi_bundle.sh):
#     sudo bash install_pi.sh
#   * matrix engine client: zaccel-bench, libzaccel (.a + .h), zaccel.py          (accel/pi)
#   * the model runtime with the Zynq offload: run_model, ppl                       (accel/llm)
#   * FPGA-GPU tools + the wired-port address + Teensy tty access (pi_setup.sh)  (accel/gpu/pi)
#   * the Teensy geometry-engine firmware, and teensy_loader_cli to flash it       (accel/gpu/teensy)
#   * the Zynq's DDR3 as swap: nbd module, nbd-client, zaccel-swap service         (accel/mem)
#   * the NVMe drive: prepared if blank, mounted at /mnt/nvme at every boot        (accel/pi/nvme-auto)
# Idempotent. Every step reports; the summary at the end says what is and is not reachable yet.
set -uo pipefail
A=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "run as root: sudo bash $0"; exit 1; }
# a bundle packed on Windows carries no executable bits: restore them on everything that runs
chmod +x "$A"/gpu/pi/build/*/gpu_* "$A"/pi/out/aarch64/* "$A"/llm/out/aarch64/* "$A"/*.sh \
         "$A"/gpu/pi/*.sh "$A"/mem/*.sh "$A"/mem/zaccel-swap "$A"/pi/nvme-auto "$A"/pi/accel-firstboot.sh 2>/dev/null
fail=0
say() { printf '\n== %s\n' "$*"; }

say "packages"
export DEBIAN_FRONTEND=noninteractive
# the Pi's Ethernet goes to the Zynq, so it may have no internet: the two missing packages ship in the
# bundle as the official arm64 .debs (pi/debs/README.md), checked against their recorded hashes
if [ "$(dpkg --print-architecture 2>/dev/null)" = arm64 ] && [ -f "$A/pi/debs/SHA256SUMS" ]; then
  if (cd "$A/pi/debs" && sha256sum -c --quiet SHA256SUMS); then
    need=(); command -v nbd-client >/dev/null || need+=("$A"/pi/debs/nbd-client_*.deb)
    command -v teensy_loader_cli >/dev/null || need+=("$A"/pi/debs/teensy-loader-cli_*.deb)
    if [ ${#need[@]} = 0 ]; then echo "nbd-client and teensy-loader-cli already installed"
    elif dpkg -i "${need[@]}" >/tmp/accel-dpkg.log 2>&1; then echo "installed from the bundle: ${need[*]##*/}"
    else echo "dpkg -i failed -- /tmp/accel-dpkg.log:"; tail -5 /tmp/accel-dpkg.log; fi
  else
    echo "bundled .debs do not match pi/debs/SHA256SUMS -- not installing them"
  fi
fi
if ! command -v nbd-client >/dev/null || ! command -v teensy_loader_cli >/dev/null || ! command -v python3 >/dev/null; then
  apt-get install -y -q teensy-loader-cli nbd-client python3 >/tmp/accel-apt.log 2>&1 \
    && echo "teensy-loader-cli, nbd-client, python3 installed" \
    || echo "apt-get failed (offline?) -- /tmp/accel-apt.log; continuing with what is present"
fi

say "NVMe drive (prepared if blank, mounted at /mnt/nvme at every boot)"
install -D -m 0755 "$A/pi/nvme-auto" /usr/local/sbin/nvme-auto && sed -i 's/\r$//' /usr/local/sbin/nvme-auto || fail=1
install -D -m 0644 "$A/pi/nvme-auto.service" /etc/systemd/system/nvme-auto.service || fail=1
systemctl daemon-reload; systemctl enable nvme-auto.service >/dev/null 2>&1 || fail=1
if ls /dev/nvme[0-9]n1 >/dev/null 2>&1; then
  systemctl restart nvme-auto.service || fail=1
  journalctl -u nvme-auto.service -b -n 8 --no-pager -o cat 2>/dev/null
else
  echo "no NVMe drive visible right now; nvme-auto.service is enabled and checks at every boot"
fi

say "matrix engine client (libzaccel, zaccel-bench, zaccel.py)"
install -D -m 0755 "$A/pi/out/aarch64/zaccel-bench" /usr/local/bin/zaccel-bench || fail=1
install -D -m 0644 "$A/pi/out/aarch64/libzaccel.a" /usr/local/lib/libzaccel.a || fail=1
install -D -m 0644 "$A/pi/libzaccel.h" /usr/local/include/libzaccel.h || fail=1
install -D -m 0755 "$A/pi/zaccel.py" /usr/local/lib/zaccel/zaccel.py || fail=1
ln -sf /usr/local/lib/zaccel/zaccel.py /usr/local/bin/zaccel.py
sed -i 's/\r$//' /usr/local/lib/zaccel/zaccel.py
echo "installed: /usr/local/bin/zaccel-bench, /usr/local/lib/libzaccel.a, /usr/local/include/libzaccel.h, /usr/local/bin/zaccel.py"

say "model runtime with the Zynq offload (run_model, ppl)"
install -D -m 0755 "$A/llm/out/aarch64/run_model" /usr/local/bin/run_model || fail=1
install -D -m 0755 "$A/llm/out/aarch64/ppl" /usr/local/bin/ppl || fail=1
echo "installed: /usr/local/bin/run_model, /usr/local/bin/ppl  (add --zaccel auto to offload)"

say "FPGA-GPU tools"
# ACCEL_NO_NET=1 leaves the network alone (tests on a machine whose wired port must not change)
bash "$A/gpu/pi/pi_setup.sh" ${ACCEL_NO_NET:+--no-net} || fail=1

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

install -D -m 0755 "$A/bench_day.sh" /usr/local/bin/bench-day && echo "installed: /usr/local/bin/bench-day"

say "what is reachable now"
for h in 10.20.0.2 10.77.0.2; do
  ping -c1 -W1 "$h" >/dev/null 2>&1 && echo "Zynq answers at $h" || echo "no answer from $h (fine if the Zynq is off)"
done
timeout 10 python3 /usr/local/lib/zaccel/zaccel.py 2>&1 | head -5 || true
swapon --show 2>/dev/null || true
echo
[ $fail = 0 ] && echo "ACCEL INSTALLED." || { echo "ACCEL INSTALL FINISHED WITH ERRORS (see above)"; exit 3; }
echo "Next: flash-teensy-gpu (Teensy on USB), then zaccel-bench and gpu_selftest with the Zynq up."
