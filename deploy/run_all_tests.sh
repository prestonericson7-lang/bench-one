#!/bin/bash
# run_all_tests.sh -- every automated check on the car system, one command, one verdict.
#
#   bash deploy/run_all_tests.sh          (Git Bash on the Windows build PC, from anywhere)
#
# Needs: Windows python with pyserial, arduino-cli with the teensy and esp32 cores, iverilog from
# tools/oss-cad-suite, and WSL Ubuntu-22.04 (root) with g++, qemu-system-arm, systemd and the SD image
# at /root/zynq/pz7020-starlite-sd.img. Touches no board, no USB device and no disk: images are
# built into a scratch directory and compared, the installer runs inside WSL, the Zynq boots in QEMU.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
T=$(mktemp -d)
PY=python
wsl_root() { MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-22.04 -u root -- "$@"; }
OSS=$ROOT/tools/oss-cad-suite
export PATH="$OSS/bin:$OSS/lib:$PATH"

npass=0; nfail=0; failed=()
check() {  # check <name> <command...> -- the command's own exit code is the verdict
  local name=$1; shift
  if "$@" >"$T/log" 2>&1; then npass=$((npass + 1)); printf '  PASS  %s\n' "$name"
  else nfail=$((nfail + 1)); failed+=("$name"); printf '  FAIL  %s\n' "$name"; tr -d '\0' <"$T/log" | tail -n 15 | sed 's/^/        /'; fi
}
sim() {    # sim <dir> <sources...> -- passes only if the testbench printed PASS and never FAIL
  (cd "$1" && shift && iverilog -g2012 -o "$T/sim.vvp" "$@" && vvp -n "$T/sim.vvp") >"$T/sim.log" 2>&1
  cat "$T/sim.log"; grep -q PASS "$T/sim.log" && ! grep -q FAIL "$T/sim.log"
}
image_matches() {  # image_matches <sketch> <fqbn> <artifact> -- rebuild and compare with SHA256SUMS
  arduino-cli compile --fqbn "$2" --output-dir "$T/fw_$1" "firmware/$1" || return 1
  local want got
  want=$(grep -E " \*?$3\$" deploy/firmware/SHA256SUMS | cut -c1-64)
  got=$(sha256sum "$T/fw_$1/$3" | cut -c1-64)
  echo "deployed $want"; echo "rebuilt  $got"; [ -n "$want" ] && [ "$want" = "$got" ]
}
must_fail() { ! "$@"; }
syntax() { local f; for f; do bash -n "$f" || return 1; done; }
flash_ps_checks_image() {  # no board attached: must verify the image, then stop at "no board" (exit 4)
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File deploy/firmware/flash_windows.ps1 "$1" >"$T/ps.log" 2>&1
  local rc=$?; cat "$T/ps.log"; [ $rc = 4 ] && grep -q "image ok" "$T/ps.log"
}

echo "== FPGA gateware (iverilog)"
R=firmware/rtlsdr-pentest/fpga/rtl
for tb in tb_spi_loopback tb_sdr_accel_spi tb_sdr_fft_top tb_fft_engine tb_ddc; do
  check "$tb" sim "$R" ../tb/$tb.v ddc.v fft_engine.v sdr_accel_spi.v sdr_fft_top.v spi_slave.v
done
check "tb_pl_regs (Zynq AXI register block)" sim hardware/pz7020-starlite/ps7-axi tb_pl_regs.v pl_regs.v
# every PACKAGE_PIN against the vendor's own pin tables (fan_jm1.xdc is the owner repo's file, uncommented)
PZ_BUNDLE=${PZ_BUNDLE:-/c/Users/Danie/Downloads/pz7020-bundle}
check "check_xdc (176 pins vs the vendor tables)" $PY hardware/pz7020-starlite/tools/check_xdc.py "$PZ_BUNDLE" \
  hardware/pz7020-starlite/vivado/system.xdc hardware/pz7020-starlite/ps7-axi/pz7020_ps7.xdc \
  hardware/pz7020-starlite/constraints/pz7020_starlite_board.xdc firmware/rtlsdr-pentest/fpga/vivado/sdr_accel.xdc

echo "== Python self-tests"
for f in firmware/telemetry-hub/can_bridge.py firmware/telemetry-hub/hub.py firmware/telemetry-hub/pl_regs.py \
         firmware/telemetry-hub/zynq_agent.py firmware/voice-assistant/assistant.py \
         firmware/voice-assistant/build_poi_index.py firmware/voice-assistant/intent.py firmware/sentry-camera/sentry.py; do
  check "$(basename "$f") --selftest" $PY "$f" --selftest
done
check "system_test (real hub + real agent + protocol-faithful nodes)" $PY firmware/telemetry-hub/system_test.py

echo "== Firmware: the real .ino code on the host, then both ends of every wire"
check "host firmware tests (vent-display, climate-node, can-logger)" wsl_root bash /mnt/d/espicpc/firmware/tests/host/build_and_run.sh
check "contract check (firmware output -> Pi parsers)" $PY firmware/tests/host/contract_check.py

echo "== Firmware images: rebuilt from source, byte-identical to what gets flashed"
check "car-can-logger.ino.hex" image_matches car-can-logger teensy:avr:teensy41 car-can-logger.ino.hex
check "vent-display.ino.hex" image_matches vent-display teensy:avr:teensy41 vent-display.ino.hex
check "vent-climate-node.ino.merged.bin" image_matches vent-climate-node esp32:esp32:esp32s3:CDCOnBoot=cdc vent-climate-node.ino.merged.bin
check "climate node refuses to build without CDCOnBoot=cdc" must_fail arduino-cli compile --fqbn esp32:esp32:esp32s3 --output-dir "$T/fw_guard" firmware/vent-climate-node

echo "== Deploy: bundle, installer, flashers"
check "make_bundle.sh" bash deploy/make_bundle.sh
check "shell syntax (install.sh, flash.sh, make_bundle.sh)" syntax deploy/orangepi/install.sh deploy/orangepi/flash.sh deploy/make_bundle.sh
check "flash_windows.ps1 logger (checks image, finds no board)" flash_ps_checks_image logger
check "flash_windows.ps1 climate (checks image, finds no board)" flash_ps_checks_image climate
check "Pi installer under systemd (WSL) + Zynq found by beacon" wsl_root bash /mnt/d/espicpc/deploy/orangepi/test_install_wsl.sh

echo "== Accelerators for the Orange Pi (accel/)"
check "matrix engine RTL vs its model, 4 flow-control patterns" bash accel/tb/run_tb.sh
check "real AXI DMA IP + engine + reset, server register sequence (xsim)" powershell.exe -NoProfile -ExecutionPolicy Bypass -File accel/cosim/run_cosim.ps1
check "zaccel-server: x86 + armhf, cpu + PL-model paths" wsl_root bash /mnt/d/espicpc/accel/zynq/run_tests.sh
check "Pi client, CPU baseline and bench (x86 + aarch64)" wsl_root bash /mnt/d/espicpc/accel/pi/test_pi.sh
check "Pi clients find the Zynq on either of its addresses" wsl_root bash /mnt/d/espicpc/accel/pi/test_fallback.sh
check "Zynq RAM export -> Pi swap (nbd, zaccel-swap)" wsl_root bash /mnt/d/espicpc/accel/mem/test_mem.sh
check "FPGA-GPU: RTL, daemon (x86 + ARM), geometry, Pi tools end to end" wsl_root bash /mnt/d/espicpc/accel/gpu/run_all_gpu_tests.sh

echo "== Zynq SD image"
check "image carries the repo's boot files, agent and accelerators" wsl_root bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/check_image_contents.sh
check "image boots in QEMU, agent serves the hub over eth0" wsl_root bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_agent_test.sh
check "image boots in QEMU, all accelerator services answer the Pi" wsl_root bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_accel_test.sh

rm -rf "$T"
echo
if [ $nfail = 0 ]; then echo "ALL $npass CHECKS PASSED"; exit 0; fi
echo "$nfail of $((npass + nfail)) CHECKS FAILED:"; printf '  %s\n' "${failed[@]}"; exit 1
