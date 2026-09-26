#!/bin/bash
# build_host.sh -- the repo's model runtime tools (ppl, run_model) for the x86 host in WSL, CORRECTNESS
# ONLY (a PC timing never counts here). Links libzaccel so the Zynq offload path can be checked
# against a zaccel-server on this machine. Outputs in accel/llm/out/.
set -euo pipefail
R=/mnt/d/espicpc
S=$R/firmware/bench-one/shared
T=$R/firmware/bench-one/tests
O=$R/accel/llm/out; mkdir -p "$O"
SRC="$S/gguf.c $S/gguf_dot.c $S/model_q.c $S/tokenizer.c $R/accel/pi/libzaccel.c $R/accel/llm/zaccel_offload.c"
[ -f "$S/gguf_bits.c" ] && SRC="$SRC $S/gguf_bits.c"
CF="-O2 -fopenmp -I$S -I$R/accel/pi -DZACCEL_OFFLOAD -lm -lpthread"
gcc $CF -o "$O/ppl"       "$T/ppl.c"       $SRC -lm -lpthread
gcc $CF -o "$O/run_model" "$T/run_model.c" $SRC -lm -lpthread
ls -la "$O"
