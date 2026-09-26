#!/bin/bash
# run_tb.sh -- the matrix engine against its independent model under four flow-control patterns
# (no gaps; moderate; heavy; output almost always stalled). Git Bash on the build PC.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE"
OSS=$(cd "$HERE/../../tools/oss-cad-suite" && pwd)
export PATH="$OSS/bin:$OSS/lib:$PATH"
python gen_vectors.py 1 >/dev/null || exit 1
iverilog -g2012 -o tb.vvp tb_zaccel_gemv.v ../rtl/zaccel_gemv.v || exit 1
rc=0
for cfg in "0 0 1" "30 50 2" "70 90 3" "0 95 4"; do
  set -- $cfg
  out=$(vvp -n tb.vvp +vgap=$1 +stall=$2 +seed=$3 | grep -E "ALL PASS|FAIL")
  echo "$out" | tail -n 1
  echo "$out" | grep -q "ALL PASS" || rc=1
done
rm -f tb.vvp
exit $rc
