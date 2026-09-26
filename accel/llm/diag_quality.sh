#!/bin/bash
# diag_quality.sh -- which half of the offload's approximation costs the quality: every row offloaded,
# computed three ways (full: int8 weights x int8 activation on the engine; wonly: int8 weights only;
# aonly: int8 activation only). WSL.
set -u
H=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-/mnt/d/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba}
LIMIT=${1:-64}; PORT=18195
BIN=${BIN:-/tmp/ppl_diag}
R=/mnt/d/espicpc; S=$R/firmware/bench-one/shared
gcc -O2 -fopenmp -I$S -I$R/accel/pi -DZACCEL_OFFLOAD -o $BIN $R/firmware/bench-one/tests/ppl.c $S/gguf.c $S/gguf_dot.c \
    $S/model_q.c $S/tokenizer.c $S/gguf_bits.c $R/accel/pi/libzaccel.c $R/accel/llm/zaccel_offload.c -lm -lpthread 2>/dev/null || exit 1
"$H/../zynq/out/zaccel-server-x86" --cpu --cpu-mb 3600 --port $PORT >/tmp/zd_server.log 2>&1 &
SV=$!; sleep 1
run() { "$BIN" "$MODEL" --fast --limit "$LIMIT" --zaccel 127.0.0.1:$PORT --share 1.0 2>&1 | grep -E "PERPLEXITY|top-1"; }
echo "== baseline (no offload)"; "$BIN" "$MODEL" --fast --limit "$LIMIT" 2>&1 | grep -E "PERPLEXITY|top-1"
echo "== all rows: int8 weights x int8 activation (the engine)"; run
echo "== all rows: int8 weights only";     ZACCEL_EXP=wonly run
echo "== all rows: int8 activation only";  ZACCEL_EXP=aonly run
kill $SV 2>/dev/null
