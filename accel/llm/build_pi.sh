#!/bin/bash
# build_pi.sh -- the model runtime tools with the Zynq offload, static aarch64 for the Orange Pi
# (A733: Cortex-A76 + A55, ARMv8.2-A with dot product). Outputs in accel/llm/out/aarch64/.
#   WSL:  bash build_pi.sh
set -euo pipefail
R=/mnt/d/espicpc
S=$R/firmware/bench-one/shared
T=$R/firmware/bench-one/tests
O=$R/accel/llm/out/aarch64; mkdir -p "$O"
SRC="$S/gguf.c $S/gguf_dot.c $S/model_q.c $S/tokenizer.c $R/accel/pi/libzaccel.c $R/accel/llm/zaccel_offload.c"
[ -f "$S/gguf_bits.c" ] && SRC="$SRC $S/gguf_bits.c"
CF="-O3 -march=armv8.2-a+dotprod -fopenmp -static -I$S -I$R/accel/pi -DZACCEL_OFFLOAD -DZACCEL_NO_RESOLVER"
for t in ppl run_model; do
  aarch64-linux-gnu-gcc $CF -o "$O/$t" "$T/$t.c" $SRC -lm -lpthread
done
file "$O"/* | sed 's/,.*static/, static/'
ls -la "$O"
