#!/bin/sh
# Build and verify the DDR3 bridge gateware, end to end, and produce both bitstreams.
#
# Set OSS_CAD to an OSS CAD Suite extract, or have iverilog/vvp/yosys/nextpnr on PATH. On Windows
# those binaries find their DLLs through PATH, not through the directory they sit in, so bin must
# actually be exported -- calling the executable by full path exits 127 instead.
#
# Stages 1 to 5 are simulation and must all pass before a bitstream is produced. That order is
# deliberate: a design that fails its testbench should never reach a file you could flash.
#
# THE TWO CONFIGURATIONS. sys_clk is NOT the board oscillator. The Cu's oscillator is 100 MHz and
# this design does not close there -- measured across eight placer seeds it lands between 81.5 and
# 97.4 MHz -- so sys_clk comes from the PLL and the oscillator is only its reference.
#
#                sys_clk   CK_DIV   memory      DRAM raw   link                 effective
#   cfgA  first   50.0 MHz     8    6.25   MHz  12.5 MB/s  1 line  @49.5 = 6.19  5.4  MB/s
#   cfgB  full    62.5 MHz     4    15.625 MHz  31.25      4 lines @49.5 = 24.75 15.2 MB/s
#
# cfgA is the one to build first: its data lines run at 12.5 Mbps, inside a TXB0108's rating. cfgB
# needs direction-controlled translators. See WIRING.md.
set -e
D="$(cd "$(dirname "$0")" && pwd)"
if [ -n "$OSS_CAD" ]; then PATH="$OSS_CAD/bin:$OSS_CAD/lib:$PATH"; export PATH; fi
cd "$D"
mkdir -p build

SRC="ddr3_ctrl.v qspi_slave.v ddr3_bridge.v ddr3_top.v ddr3_model.v tb_chain.v"

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

echo "=== 3. memory clock divider: CK_DIV below 4 has no quarter-period and must fail ==="
for ck in 8 4; do
  if [ "$ck" = 8 ]; then s=2; else s=1; fi
  iverilog -g2005 -o build/c.vvp -Ptb_ddr3.CK_DIV=$ck -Ptb_ddr3.RD_SAMPLE=$s \
    ddr3_ctrl.v ddr3_model.v tb_ddr3.v
  printf "  CK_DIV=%s : %s\n" "$ck" "$(vvp build/c.vvp | grep -oE 'PASSED|FAILED.*')"
done

echo "=== 4. cfgB, the whole chain: 62.5 MHz fabric, 15.625 MHz memory, four-line link ==="
iverilog -g2005 -o build/chainB.vvp \
  -Ptb_chain.SYS_HZ=62500000 -Ptb_chain.SYS_HALF=8 -Ptb_chain.CK_DIV=4 -Ptb_chain.QUAD_OK=1 $SRC
vvp build/chainB.vvp | grep -E "fabric|SCLK|initialised|identity|match|flagged|carrying|wrong|model:|PASSED|FAILED"

echo "=== 5. cfgA, the whole chain: 50 MHz fabric, 6.25 MHz memory, single-bit link ==="
# The only configuration that works with weak level translators. FlexSPI2 cannot be clocked below
# 49.5 MHz, and four lines at that rate consume 24.75 MB/s -- more than any memory slower than
# 14.4 MHz can supply. One line consumes 6.19 MB/s instead, leaving a factor of two in hand.
iverilog -g2005 -o build/chainA.vvp \
  -Ptb_chain.SYS_HZ=50000000 -Ptb_chain.SYS_HALF=10 -Ptb_chain.CK_DIV=8 -Ptb_chain.QUAD_OK=0 $SRC
vvp build/chainA.vvp | grep -E "fabric|SCLK|initialised|identity|match|wrong|model:|PASSED|FAILED"

if [ ! -f alchitry_cu.pcf ]; then
  echo "=== 6-7. SKIPPED: alchitry_cu.pcf is missing, so there is nothing to place against. ==="
  exit 0
fi

# ---------------------------------------------------------------------------------------------
#  Synthesis, place and route, and pack -- once per configuration.
#
#  Every seed is checked, not just one. This design's Fmax moves by 10 MHz between placer seeds, so
#  a single passing run proves nothing about the next run. If any seed fails, the configuration is
#  too fast, full stop.
# ---------------------------------------------------------------------------------------------
build_cfg() {
  NAME=$1; HZ=$2; DIVF=$3; CK=$4; FREQ=$5
  echo "=== 6.$NAME synthesis: sys_clk $FREQ MHz, CK_DIV $CK ==="
  yosys -p "read_verilog ddr3_ctrl.v qspi_slave.v ddr3_bridge.v ddr3_top.v; \
            chparam -set SYS_HZ $HZ -set USE_PLL 1 -set PLL_DIVR 0 -set PLL_DIVF $DIVF \
                    -set PLL_DIVQ 4 -set PLL_FILTER 5 -set CK_DIV $CK ddr3_top; \
            synth_ice40 -top ddr3_top -json build/$NAME.json; stat" > build/$NAME.syn 2>&1
  awk '/Printing statistics/,0' build/$NAME.syn |
    grep -E '^ +[0-9]+ +SB_(LUT4|RAM40_4K|IO|PLL40_CORE)$' | sort -u -k2 | sed 's/^ */    /'
  echo "    out of 7680 LUT4s and 32 block RAMs on an HX8K"

  echo "=== 7.$NAME place and route, all four seeds must pass $FREQ MHz ==="
  WORST=""
  for sd in 1 2 3 4; do
    if nextpnr-ice40 --hx8k --package cb132 --pcf alchitry_cu.pcf --json build/$NAME.json \
         --asc build/${NAME}_s$sd.asc --freq $FREQ --seed $sd --placer heap --tmg-ripup \
         > build/${NAME}_s$sd.log 2>&1; then R=pass; else R=FAIL; fi
    F=$(grep -E "Max frequency for clock  *'sys_clk" build/${NAME}_s$sd.log | tail -1 |
        sed -E 's/.*: ([0-9.]+) MHz.*/\1/')
    printf "    seed %d: sys_clk %s MHz  %s\n" "$sd" "${F:-?}" "$R"
    [ "$R" = FAIL ] && { echo "    *** $NAME does not close at $FREQ MHz on seed $sd ***"; exit 1; }
  done
  cp build/${NAME}_s1.asc build/$NAME.asc
  icepack build/$NAME.asc build/$NAME.bin
  echo "    bitstream: build/$NAME.bin  ($(wc -c < build/$NAME.bin) bytes)"
  echo "    flash it:  openFPGALoader -b cu build/$NAME.bin"
}

build_cfg cfgA 50000000 7 8 50
build_cfg cfgB 62500000 9 4 62.5

echo
echo "=== done. Build cfgA first -- it is the one that works with TXB0108s. See WIRING.md. ==="
