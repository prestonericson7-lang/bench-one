#!/bin/sh
# Build and verify the DDR3 bridge gateware.
#
# Set OSS_CAD to an OSS CAD Suite extract, or have iverilog/vvp/yosys on PATH. On Windows those
# binaries find their DLLs through PATH, not through the directory they sit in, so bin must actually
# be exported -- calling the executable by full path exits 127 instead.
set -e
D="$(cd "$(dirname "$0")" && pwd)"
if [ -n "$OSS_CAD" ]; then PATH="$OSS_CAD/bin:$OSS_CAD/lib:$PATH"; export PATH; fi
cd "$D"
mkdir -p build

echo "=== 1. DDR3 controller against a protocol-checking device model ==="
iverilog -g2005 -o build/tb.vvp ddr3_ctrl.v ddr3_model.v tb_ddr3.v
vvp build/tb.vvp | grep -E "tCK|init complete|wrote|read back|model:|PASSED|FAILED|\*\*\*"

echo "=== 2. read calibration: every arrival time must be tunable, since DQS is unusable ==="
for case in "10 6 2" "11 6 6" "12 7 2" "13 7 6" "14 8 2"; do
  set -- $case
  iverilog -g2005 -o build/s.vvp \
    -Ptb_ddr3.RD_DELAY_HALF=$1 -Ptb_ddr3.RD_LATENCY=$2 -Ptb_ddr3.RD_SAMPLE=$3 \
    ddr3_ctrl.v ddr3_model.v tb_ddr3.v
  printf "  device %2s half-clocks late, RD_LATENCY=%s RD_SAMPLE=%s : %s\n" \
    "$1" "$2" "$3" "$(vvp build/s.vvp | grep -oE 'PASSED|FAILED.*')"
done

echo "=== 3. memory clock: 25 MHz is the ceiling for this single-edge design ==="
for ck in 8 4; do
  if [ "$ck" = 8 ]; then s=2; else s=1; fi
  iverilog -g2005 -o build/c.vvp -Ptb_ddr3.CK_DIV=$ck -Ptb_ddr3.RD_SAMPLE=$s \
    ddr3_ctrl.v ddr3_model.v tb_ddr3.v
  printf "  CK_DIV=%s : %s\n" "$ck" "$(vvp build/c.vvp | grep -oE 'PASSED|FAILED.*')"
done

echo "=== 4. synthesis for iCE40-HX8K ==="
yosys -p "read_verilog ddr3_ctrl.v qspi_slave.v ddr3_bridge.v ddr3_top.v; \
          synth_ice40 -top ddr3_top -json build/ddr3.json; stat" > build/synth.log 2>&1
awk '/Printing statistics/,0' build/synth.log |
  grep -E '^ +[0-9]+ +SB_(LUT4|RAM40_4K|IO)$' | sort -u -k2 | sed 's/^ */  /'
echo "  out of 7680 LUT4s and 16 block RAMs on an HX8K"

echo "=== 5. place and route ==="
if [ ! -f alchitry_cu.pcf ]; then
  echo "  SKIPPED: alchitry_cu.pcf does not exist."
  echo "  Copy alchitry_cu.pcf.template and fill in the ball names from Alchitry's own cu.pcf."
  echo "  The pin letters are deliberately not guessed: a wrong ball on a DQ line shorts a 1.5 V"
  echo "  part to a 3.3 V driver."
  exit 0
fi
# NOTE: nextpnr-ice40 in some OSS CAD Suite extracts aborts at startup with
#   "failed to get the Python codec of the filesystem encoding"
# because the bundled Python standard library is missing (no python311.zip, and lib/python3.11
# contains only site-packages). That is an install defect, not a design problem -- reinstall the
# suite or use a distribution package of nextpnr.
nextpnr-ice40 --hx8k --package cb132 --pcf alchitry_cu.pcf \
  --json build/ddr3.json --asc build/ddr3.asc --freq 100
icepack build/ddr3.asc build/ddr3.bin
echo "  bitstream at build/ddr3.bin"
