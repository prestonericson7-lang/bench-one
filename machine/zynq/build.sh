#!/bin/bash
# build.sh -- the model runtime and the engine tools for the Zynq's OWN processor (Cortex-A9, armhf),
# so the machine can run and benchmark itself with no Pi and no PC in the loop.
#   out/armhf/run_model     the model runner with the Zynq offload (--zaccel HOST[,HOST2...])
#   out/armhf/ppl           perplexity with the same offload
#   out/armhf/zaccel-bench  the engine's own bit-exact check + rate (accel/pi)
#   out/armhf/test_lib      libzaccel's tests against a live server
#   out/armhf/tl_ref        the exact reference (the Teensy's proof) on the Zynq's CPU
# Static, NEON. Run in WSL Ubuntu-22.04:  bash /mnt/d/espicpc/machine/zynq/build.sh
set -euo pipefail
R=${REPO:-/mnt/d/espicpc}
S=$R/firmware/bench-one/shared
T=$R/firmware/bench-one/tests
O=$R/machine/zynq/out/armhf; mkdir -p "$O"
CC=arm-linux-gnueabihf-gcc
# Cortex-A9 with NEON + VFPv3 (the Zynq 7000's PS). -O3 -fopenmp as the Pi build; static so the
# Debian rootfs on the card needs nothing else.
CF="-O3 -std=gnu11 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fopenmp -static -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -I$S -I$R/accel/pi"
# NOTE: static glibc cannot resolve host NAMES without matching NSS libraries on the target, so every
# tool is given numeric addresses (machine-bench resolves zynqN with the board's own getent first).
MODELSRC="$S/gguf.c $S/gguf_dot.c $S/gguf_bits.c $S/model_q.c $S/tokenizer.c"
OFF="$R/accel/pi/libzaccel.c $R/accel/llm/zaccel_offload.c"
for t in run_model ppl; do
  $CC $CF -DZACCEL_OFFLOAD -DZACCEL_NO_RESOLVER -o "$O/$t" "$T/$t.c" $MODELSRC $OFF -lm -lpthread
done
# the exact reference (tl_ref: no offload, the digits the Teensy must match)
$CC $CF -o "$O/tl_ref" "$T/tl_ref.c" $MODELSRC -lm -lpthread
# the engine tools (accel/pi)
( cd "$R/accel/pi" && $CC $CF -o "$O/zaccel-bench" zaccel-bench.c libzaccel.c zaccel_cpu.c -lm -lpthread \
                   && $CC $CF -o "$O/test_lib" test_lib.c libzaccel.c zaccel_cpu.c -lm -lpthread )
file "$O"/* | sed 's/,.*static/, static/'
ls -la "$O"
( cd "$O" && sha256sum * > SHA256SUMS )
echo "BUILD OK: $O"
