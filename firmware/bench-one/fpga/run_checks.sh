#!/bin/sh
# Verify everything in fpga/: simulate the RTL, cross-check the design, then synthesise.
#
# Set OSS_CAD to the root of an OSS CAD Suite extract, or have iverilog/vvp/yosys on PATH.
# On Windows those binaries find their DLLs through PATH, not through the directory they sit in,
# so bin must actually be exported -- calling the executable by full path exits 127 instead.
set -e
D="$(cd "$(dirname "$0")" && pwd)"
if [ -n "$OSS_CAD" ]; then PATH="$OSS_CAD/bin:$OSS_CAD/lib:$PATH"; export PATH; fi
cd "$D/tb"

echo "=== 1. hd_scan design cross-check against bench_hdc_shard.c ==="
python "$D/tb/hd_scan_model.py" | grep -E "trials|VERIFIED|FAILURES"

echo "=== 2. hd_scan RTL ==="
iverilog -g2005 -o tb.vvp ../rtl/hd_popcount.v ../rtl/hd_scan.v tb_hd_scan.v && vvp tb.vvp | tail -3

echo "=== 3. gemv_int4 RTL -- one row ==="
iverilog -g2005 -o tbg.vvp ../rtl/gemv_int4.v tb_gemv_int4.v && vvp tbg.vvp | tail -3

echo "=== 4. gemm_int4 RTL -- a whole layer ==="
iverilog -g2005 -o tbm.vvp ../rtl/gemv_int4.v ../rtl/gemm_int4.v tb_gemm_int4.v \
  && vvp tbm.vvp | grep -E "rows correct|beats|duty|PASSED|FAILED"

echo "=== 5. synthesis for xc7 ==="
for L in 8 32 64; do
  yosys -p "read_verilog ../rtl/gemv_int4.v ../rtl/gemm_int4.v; \
            chparam -set LANES $L gemm_int4; hierarchy -top gemm_int4; \
            synth_xilinx -family xc7 -flatten; stat" 2>&1 |
    sed -n '/=== gemm_int4 ===/,/Estimated number of LCs/p' |
    grep -E "Estimated" | sed "s/^/  LANES=$L /"
done
