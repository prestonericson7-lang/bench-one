#!/bin/bash
# run_tests.sh -- the Zynq offload in the model runtime, judged three ways (WSL, correctness only):
#   1. kernels: offload vs CPU on real matrices, decode + prefill, relative error < 3%
#   2. text:    the model's greedy output with half the rows offloaded equals CPU-only
#   3. quality: perplexity with half and with all rows offloaded within 3% of CPU-only (ppl.c)
# Needs a GGUF model: $MODEL, default the local Ollama qwen2.5-coder:3b blob. Absent -> SKIP (exit 0).
set -u
H=$(cd "$(dirname "$0")" && pwd)
export MODEL=${MODEL:-/mnt/d/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba}
[ -f "$MODEL" ] || { echo "SKIP: no model at $MODEL (set MODEL=<gguf>)"; exit 0; }
R=/mnt/d/espicpc; S=$R/firmware/bench-one/shared
bash "$H/build_host.sh" >/tmp/llm_build.log 2>&1 || { tail -5 /tmp/llm_build.log; echo "FAIL build"; exit 1; }
gcc -O2 -fopenmp -I$S -I$R/accel/pi -DZACCEL_OFFLOAD -o "$H/out/test_kernels" "$H/test_kernels.c" $S/gguf.c $S/gguf_dot.c \
    $S/model_q.c $S/tokenizer.c $S/gguf_bits.c $R/accel/pi/libzaccel.c $H/zaccel_offload.c -lm -lpthread || exit 1
rc=0
"$H/../zynq/out/zaccel-server-x86" --cpu --cpu-mb 3000 --port 18198 >/tmp/llm_srv.log 2>&1 & SV=$!; sleep 1
k=$("$H/out/test_kernels" "$MODEL" 127.0.0.1:18198 0.5 2>&1); echo "$k" | sed 's/^/  /'
worst=$(echo "$k" | awk '/rel err/ {for (i=1;i<=NF;i++) if ($i=="err") print $(i+1)}' | sort -g | tail -1)
awk -v w="$worst" 'BEGIN{exit !(w < 0.03)}' && echo "PASS kernels: worst relative error $worst" || { echo "FAIL kernels: worst $worst"; rc=1; }
kill $SV 2>/dev/null
g=$(bash "$H/test_generate.sh" 2>&1); echo "$g" | grep -E "zaccel:|RESULT" | sed 's/^/  /'
echo "$g" | grep -q "RESULT: identical text" && echo "PASS text: identical with half the rows offloaded" || { echo "FAIL text differs"; rc=1; }
q=$(bash "$H/test_quality.sh" 64 2>&1); echo "$q" | grep -E "==|PERPLEXITY" | sed 's/^/  /'
set -- $(echo "$q" | awk '/PERPLEXITY/ {print $2}')
if [ $# -eq 3 ] && awk -v b="$1" -v h="$2" -v a="$3" 'BEGIN{exit !(h < b*1.03 && a < b*1.03)}'; then
  echo "PASS quality: perplexity $1 -> $2 (half) / $3 (all), within 3%"
else echo "FAIL quality: perplexity $* (need three values within 3% of the first)"; rc=1; fi
d=$(bash "$H/test_dies.sh" 2>&1); drc=$?
echo "$d" | grep -E "offload off|FAIL" | sed 's/^/  /'
[ $drc = 0 ] && echo "PASS the Zynq dying mid-generation costs nothing but speed" || { echo "FAIL fallback"; rc=1; }
[ $rc = 0 ] && echo "LLM OFFLOAD TESTS: PASS" || echo "LLM OFFLOAD TESTS: FAIL"
exit $rc
