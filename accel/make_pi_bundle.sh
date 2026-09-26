#!/bin/bash
# make_pi_bundle.sh -- package what the Orange Pi needs into accel/accel-pi-bundle.tar.gz
#   (built binaries + scripts only; build everything first). Run from anywhere.
set -euo pipefail
cd "$(dirname "$0")"
need=(pi/out/aarch64/zaccel-bench pi/out/aarch64/libzaccel.a pi/libzaccel.h pi/zaccel.py
      gpu/pi/pi_setup.sh mem/install_pi.sh mem/zaccel-swap install_pi.sh README.md
      llm/out/aarch64/run_model llm/out/aarch64/ppl)
for f in "${need[@]}"; do [ -e "$f" ] || { echo "missing $f -- build first"; exit 1; }; done
[ -f gpu/teensy/out/600/teensy_gpu.ino.hex ] || { echo "missing gpu/teensy/out/600/teensy_gpu.ino.hex -- build the Teensy firmware first"; exit 1; }
out=accel-pi-bundle.tar.gz
tar -czf "$out" --exclude='__pycache__' \
  install_pi.sh README.md SPEC.md \
  pi/out/aarch64 pi/libzaccel.h pi/zaccel.py llm/out/aarch64 \
  gpu/pi gpu/teensy/out/600/teensy_gpu.ino.hex gpu/SPEC.md gpu/WIRING.md \
  mem/install_pi.sh mem/zaccel-swap mem/zaccel-swap.service mem/zaccel-swap-retry.service \
  mem/zaccel-swap-retry.timer mem/pi-kmod
ls -la "$out"; sha256sum "$out"
