#!/bin/bash
# make_bundle.sh -- package everything the Orange Pi needs into deploy/car-bundle.tar.gz
#   (hub + CAN tools + voice-assistant code + firmware images + installer). Run from the repo root.
set -euo pipefail
cd "$(dirname "$0")/.."
out=deploy/car-bundle.tar.gz
tar -czf "$out" --exclude='__pycache__' --exclude='*.pyc' \
  firmware/telemetry-hub/hub.py firmware/telemetry-hub/can_bridge.py firmware/telemetry-hub/signals.json \
  firmware/telemetry-hub/system_test.py firmware/car-can-logger/canlog.py firmware/voice-assistant \
  deploy/README.md deploy/orangepi deploy/firmware
ls -la "$out"; sha256sum "$out"
