#!/bin/bash
# check_build.sh -- compile the runtime + offload with all warnings into /tmp (does not touch out/,
# which may be in use by a running test). WSL.
R=/mnt/d/espicpc; S=$R/firmware/bench-one/shared
gcc -O2 -fopenmp -Wall -Wextra -I$S -I$R/accel/pi -DZACCEL_OFFLOAD -o /tmp/run_model_chk \
    $R/firmware/bench-one/tests/run_model.c $S/gguf.c $S/gguf_dot.c $S/model_q.c $S/tokenizer.c $S/gguf_bits.c \
    $R/accel/pi/libzaccel.c $R/accel/llm/zaccel_offload.c -lm -lpthread 2>&1 \
  | grep -E "zaccel_offload.c|model_q.c:[0-9]+:[0-9]+: (error|warning)|error" | head -20
[ -x /tmp/run_model_chk ] && echo "BUILD OK" || { echo "BUILD FAILED"; exit 1; }
