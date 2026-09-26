#!/bin/bash
# test_quality.sh -- what the Zynq offload costs the model, measured with the repo's own yardstick
# (firmware/bench-one/tests/ppl.c: perplexity on a real passage). The "Zynq" here is zaccel-server on
# this machine in --cpu mode, whose integer results are identical to the PL engine's (accel/SPEC.md),
# so the quality figure is the real one; the timings are not and are not printed.
#   MODEL=<gguf> bash test_quality.sh [limit]          (WSL)
set -u
H=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-/mnt/d/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba}
LIMIT=${1:-96}
PORT=18193
"$H/../zynq/out/zaccel-server-x86" --cpu --cpu-mb 3600 --port $PORT >/tmp/zq_server.log 2>&1 &
S=$!; sleep 1
run() { "$H/out/ppl" "$MODEL" --fast --limit "$LIMIT" "$@" 2>&1 | grep -E "zaccel:|PERPLEXITY|top-1" ; }
echo "== baseline: every row on this CPU (GGUF Q4_K/Q6_K, fused integer path)"; run
echo "== half the rows on the Zynq (int8 per-row weights, int8 activations)";    run --zaccel 127.0.0.1:$PORT --share 0.5
echo "== every row on the Zynq";                                                  run --zaccel 127.0.0.1:$PORT --share 1.0
kill $S 2>/dev/null
