#!/usr/bin/env bash
# ===========================================================================================
#  driver.sh -- build and drive BENCH ONE from a cold start
# ===========================================================================================
#
#  BENCH ONE has no window and no server. Its surface is a set of command-line programs that
#  load a real GGUF model and either generate text, measure a bottleneck, or split themselves
#  across several processes and do it together. This drives all of that.
#
#  Everything here has been run on this machine. The build lines in particular are not
#  guessable: the toolchain is not on PATH, the sources are split across two directories, one
#  of them (gguf_bits.c) exists only so microcontroller targets do not link stdio, and
#  anything touching the network needs -lws2_32 on Windows.
#
#  RUN
#      bash .claude/skills/run-bench-one/driver.sh <command>
#
#      build     compile every host binary
#      smoke     load the real model and generate text, and check it says something sane
#      dist      the same model split across 4 processes, checked against the single process
#      bench     the bottleneck measurements: memory, unpacking, the fused kernel
#      quality   perplexity, the yardstick for judging any approximation
#      plan      the capacity and throughput planner
#      boards    detect attached hardware and measure whatever is plugged in
#      graphics  render the machine's own topology, split across bands, and check the stitch
#      push      copy a file to an attached Luckfox (adb push is broken on that image)
#      all       build + smoke + dist
# ===========================================================================================

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

# Everything below is a WINDOWS path (D:/espicpc/...), never a unix one (/d/espicpc/...).
#
# This is not cosmetic. The vendored toolchain in tools/w64devkit is BusyBox-w32: its cat, grep, sed
# and diff are Windows programs, and they cannot open /d/espicpc/anything. Git Bash would normally
# translate the path on the way out, but MSYS_NO_PATHCONV=1 is set a few lines down because adb
# needs it, and that switches the translation off for every program at once. The symptom is a run
# that creates its scratch directory successfully and then reports every file in it missing.
#
# Bash reads D:/... perfectly well, so converting once here costs nothing and fixes all of it.
#
# PATH is the one exception and must stay unix-style: it is colon separated, so a D:/... entry is
# read as the directory "D" followed by "/espicpc/...", and gcc stops being findable.
ROOT_UNIX="$ROOT"
if command -v cygpath >/dev/null 2>&1; then
    HERE="$(cygpath -m "$HERE")"
    ROOT="$(cygpath -m "$ROOT")"
fi
TESTS="$ROOT/firmware/bench-one/tests"
SHARED="$ROOT/firmware/bench-one/shared"

# The compiler is vendored, not installed. Nothing builds without this line.
export PATH="$ROOT_UNIX/tools/w64devkit/bin:$PATH"
# Git Bash rewrites anything that looks like a unix path before handing it to a Windows exe,
# which turns a board's /tmp/foo into C:/Users/.../Temp/foo. Fatal for adb, harmless otherwise.
export MSYS_NO_PATHCONV=1

# A real model, not a fixture. Override with MODEL=/path/to/x.gguf.
MODEL="${MODEL:-D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba}"

# Source paths are RELATIVE and every compile happens from the tests directory.
#
# Not a style choice. gcc here is a Windows executable, and with MSYS_NO_PATHCONV=1 set (which adb
# needs) Git Bash stops rewriting /d/espicpc/... into D:/espicpc/..., so an absolute unix path
# reaches the compiler verbatim and it reports the file as missing. Relative paths sidestep the
# whole question.
#
# gguf_bits.c holds the format arithmetic with no file code attached, so a microcontroller can link
# the kernels without linking stdio. On the host it is simply a fourth source file, and leaving it
# out gives undefined references to gguf_dequant.
CORE="../shared/gguf.c ../shared/gguf_bits.c ../shared/gguf_dot.c"
MODELSRC="../shared/model_q.c ../shared/tokenizer.c"
# NO -mavx2, DELIBERATELY.
#
# This PC is not a node in the machine and none of the boards have its vector units, so a host
# number produced with them is not a figure about BENCH ONE. The x86 kernel path is guarded by
# __AVX2__ and compiles out entirely without this flag, which is the point: the host build cannot
# quietly produce a flattering number that later gets mistaken for the unit.
#
# The host binaries exist to check CORRECTNESS -- that the distributed split matches one process,
# that a kernel is bit-identical to the reference. Speed is measured on boards or not at all.
CFLAGS="-O3 -fopenmp -std=c11 -Wall -Wextra -I../shared"

SCRATCH="$HERE/scratch"
mkdir -p "$SCRATCH" 2>/dev/null

# Every command makes its own scratch directory. Doing it once at the top is not enough: a run
# that fails part way can leave it gone, and the next redirect into it fails obscurely.
say() { mkdir -p "$SCRATCH" 2>/dev/null; printf '\n=== %s ===\n' "$*"; }
die() { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

need_model() {
    [ -f "$MODEL" ] || die "no model at $MODEL -- set MODEL=/path/to/a.gguf"
}

# -------------------------------------------------------------------------------------------
cmd_build() {
    say "building host binaries"
    cd "$TESTS" || die "no $TESTS"

    # -lws2_32 is required by anything including stage_link: Windows sockets are not in libc.
    gcc $CFLAGS -o run_model.exe    run_model.c    $CORE $MODELSRC -lm \
        || die "run_model"
    gcc $CFLAGS -o ppl.exe          ppl.c          $CORE $MODELSRC -lm \
        || die "ppl"
    gcc $CFLAGS -o pipe_model.exe   pipe_model.c   $CORE $MODELSRC \
        ../shared/stage_link.c -lws2_32 -lm || die "pipe_model"
    gcc $CFLAGS -o decode_limit.exe decode_limit.c $CORE -lm || die "decode_limit"
    gcc $CFLAGS -o fast_path.exe    fast_path.c    $CORE -lm || die "fast_path"
    gcc $CFLAGS -o gguf_load.exe    gguf_load.c    $CORE -lm || die "gguf_load"
    gcc -O2 -std=c11 -Wall -I../shared -o hop_bench.exe hop_bench.c ../shared/stage_link.c -lws2_32 \
        || die "hop_bench"
    # No -fopenmp here: the renderer is measured single threaded, because that is what a board is.
    # -I../shared: the rasteriser lives in machine_scene.h so the Teensy and the Luckfox compile the
    # same file rather than a port of it.
    gcc -O3 -std=c11 -Wall -Wextra -I../shared -o machine_view.exe machine_view.c -lm || die "machine_view"
    gcc -O3 -std=c11 -Wall -Wextra -I../shared -o dot_verify.exe dot_verify.c ../shared/gguf.c ../shared/gguf_bits.c ../shared/gguf_dot.c -lm || die "dot_verify"
    gcc -O3 -std=c11 -Wall -Wextra -o gfx_bench.exe gfx_bench.c -lm || die "gfx_bench"
    gcc -O2 -std=c11 -Wall -Wextra -o disk_stream.exe disk_stream.c || die "disk_stream"
    gcc $CFLAGS -o moe_route.exe moe_route.c $CORE $MODELSRC -lm || die "moe_route"

    # The ARM builds are what run on a Luckfox. Static, because the board's libc is not this one.
    local ARM="$ROOT/tools/arm-linux-gnueabihf/bin/arm-none-linux-gnueabihf-gcc.exe"
    if [ -x "$ARM" ]; then
        # -mfpu=neon is the whole point of the ARM build. Every weight-carrying node in this
        # machine is ARM or fabric, so this is the only vector path that is a figure ABOUT the
        # machine. Without the flag __ARM_NEON is undefined and the boards run scalar code.
        local AF="-O3 -std=gnu11 -static -march=armv7-a -mfpu=neon -mfloat-abi=hard -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -I../shared"
        "$ARM" $AF -o decode_limit_arm decode_limit.c $CORE -lm || die "decode_limit_arm"
        "$ARM" $AF -o hop_bench_arm    hop_bench.c    ../shared/stage_link.c || die "hop_bench_arm"
        # -std=gnu11, not c11: clock_gettime is hidden by strict c11 and now_s() will not compile.
        "$ARM" $AF -o machine_view_arm machine_view.c -lm || die "machine_view_arm"
        "$ARM" $AF -o dot_verify_arm   dot_verify.c   $CORE -lm || die "dot_verify_arm"
        "$ARM" $AF -o disk_stream_arm  disk_stream.c  || die "disk_stream_arm"
        echo "  ARM binaries built too"
    fi
    ls -1 *.exe | tr '\n' ' '; echo
}

# -------------------------------------------------------------------------------------------
cmd_smoke() {
    need_model
    say "smoke: a real model, real tokens"
    cd "$TESTS" || die "no $TESTS"
    [ -x ./run_model.exe ] || cmd_build

    local OUT
    OUT=$(./run_model.exe "$MODEL" "The capital of France is" 10 --fast 2>&1)
    echo "$OUT" | grep -E "tokenizer roundtrip|loaded|decode \(memory" | sed 's/^/  /'
    echo "$OUT" | grep "^  The capital" | sed 's/^/  /'

    # Coherence is the only check that catches a wrong unpack, a wrong rotary pairing and a
    # mis-mapped attention head all at once, because each of those produces fluent nonsense at
    # exactly the right speed.
    echo "$OUT" | grep -qi "Paris" || die "the model did not say Paris -- something is wrong in the maths"
    echo "$OUT" | grep -q "roundtrip: 5 of 5" || die "tokenizer roundtrip is not exact"
    echo "  OK: says Paris, tokenizer exact"
}

# -------------------------------------------------------------------------------------------
cmd_dist() {
    need_model
    say "distributed: the same model across 4 processes"
    cd "$TESTS" || die "no $TESTS"
    [ -x ./pipe_model.exe ] || cmd_build

    local PROMPT="Write a function that reverses a linked list in place."

    # Four processes on eight cores. Without this they each grab eight threads and fight.
    export OMP_NUM_THREADS=2

    ./run_model.exe "$MODEL" "$PROMPT" 24 --fast 2>&1 | grep "^  Write a function" > "$SCRATCH/ref.txt"

    # Stages first, head last: the head closes the ring, and a stage that starts after it will
    # never be connected to.
    local i
    for i in 1 2 3; do
        ./pipe_model.exe stage "$MODEL" 4 $i 127.0.0.1 --fast --streams 1 --batch 24 \
            > "$SCRATCH/stage$i.log" 2>&1 &
    done
    sleep 3
    ./pipe_model.exe head "$MODEL" 4 127.0.0.1 "$PROMPT" 24 --fast --streams 1 --batch 24 2>&1 \
        | grep "^  Write a function" > "$SCRATCH/dist.txt"
    wait

    echo "  one process : $(cat "$SCRATCH/ref.txt")"
    echo "  four nodes  : $(cat "$SCRATCH/dist.txt")"
    diff -q "$SCRATCH/ref.txt" "$SCRATCH/dist.txt" >/dev/null 2>&1 \
        || die "distributed output differs from the single process"
    echo "  OK: byte for byte identical, so every piece of the split is correct"
}

# -------------------------------------------------------------------------------------------
cmd_bench() {
    say "what limits decode on this machine"
    cd "$TESTS" || die "no $TESTS"
    [ -x ./decode_limit.exe ] || cmd_build
    ./decode_limit.exe 512 2>&1 | grep -E "GB/s|G MAC|VERDICT|ceiling" | sed 's/^/  /'
}

cmd_quality() {
    need_model
    say "perplexity, the yardstick for judging an approximation"
    cd "$TESTS" || die "no $TESTS"
    [ -x ./ppl.exe ] || cmd_build
    local f
    for f in "--kv-f32" "" ; do
        ./ppl.exe "$MODEL" --fast $f 2>&1 | grep -E "KV cache:|PERPLEXITY" | tr -s ' ' | tr '\n' ' '
        echo
    done
}

cmd_plan() {
    say "planner"
    cd "$TESTS" || die "no $TESTS"
    python plan.py --model qwen2.5-coder-3b --ctx 1792 2>&1 | tail -40
}

# -------------------------------------------------------------------------------------------
cmd_boards() {
    say "attached hardware"
    local ADB="$ROOT/tools/platform-tools/adb.exe"
    if [ -x "$ADB" ]; then
        echo "  adb devices:"
        "$ADB" devices 2>&1 | tail -n +2 | grep -v '^$' | sed 's/^/    /'
    fi
    local CLI="/c/Program Files/Arduino CLI/arduino-cli.exe"
    if [ -x "$CLI" ]; then
        echo "  arduino-cli sees:"
        "$CLI" board list 2>&1 | grep -viE "^Port|bluetooth|^$" | sed 's/^/    /'
    fi
    echo
    echo "  A Luckfox needs its binary pushing by hand -- adb push on that image reports success"
    echo "  and writes nothing, and adb exec-out returns zero bytes. Use:"
    echo "    driver.sh push <local-file> /tmp/name     (verified by md5 at both ends)"
    echo "  which does this:"
    echo "    $ADB forward tcp:9102 tcp:9102"
    echo "    (send a python listener as one base64 line, run it as a background task, connect)"
}

# -------------------------------------------------------------------------------------------
#  Getting a file onto a Luckfox.
#
#  adb push on this board's image returns success and writes nothing. adb exec-out returns zero
#  bytes. adb reverse says "error: closed". busybox has no nc. What does work is a listener on the
#  board and adb forward, which is what this is.
cmd_push() {
    local SRC="${2:-}" DST="${3:-/tmp/mv}"
    [ -f "$SRC" ] || die "usage: driver.sh push <local-file> [/tmp/dest]"
    local ADB="$ROOT/tools/platform-tools/adb.exe"
    local DEV; DEV=$("$ADB" devices | sed -n '2p' | cut -f1)
    [ -n "$DEV" ] || die "no adb device"

    "$ADB" -s "$DEV" shell "cat > /tmp/rx.py <<'EOF'
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((\"\", 9360)); s.listen(1)
c, _ = s.accept()
open(\"$DST\",\"wb\").write(c.makefile(\"rb\").read())
c.close()
EOF
(setsid python3 /tmp/rx.py >/tmp/rx.log 2>&1 &); sleep 1; echo listening" </dev/null

    "$ADB" -s "$DEV" forward tcp:9360 tcp:9360 >/dev/null
    python -c "
import socket, sys, time
d = open(sys.argv[1],'rb').read()
for _ in range(10):
    try:
        s = socket.create_connection(('127.0.0.1', 9360), 5); break
    except OSError: time.sleep(0.5)
else: raise SystemExit('board listener never came up')
s.sendall(d); s.shutdown(socket.SHUT_WR); s.close(); print('  sent %d bytes' % len(d))
" "$SRC" || die "transfer"
    sleep 2
    echo "  local  $(md5sum "$SRC" | cut -c1-32)"
    echo "  board  $("$ADB" -s "$DEV" shell "chmod +x $DST; md5sum $DST" </dev/null | cut -c1-32)"
}

# -------------------------------------------------------------------------------------------
cmd_graphics() {
    say "the idle boards render the machine, and the bands stitch exactly"
    cd "$TESTS" || die "no $TESTS"
    [ -x ./machine_view.exe ] || cmd_build

    ROWPROFILE="$SCRATCH/rows.txt" ./machine_view.exe 480 320 120 "$SCRATCH/whole.ppm" \
        | grep -E "frames/s|pixels/s|boards," | sed 's/^/  /'

    # The whole point: 33 nodes each render their own rows and the result is one frame, byte for
    # byte. A seam of a single pixel fails this, which is how the truncation bug was caught.
    local i
    for i in $(seq 0 32); do
        ./machine_view.exe 480 320 120 "$SCRATCH/b_$i.ppm" $i 33 >/dev/null || die "band $i"
    done
    python -c "
import sys
d = sys.argv[1]
ref = open(d + '/whole.ppm','rb').read()
out = b'P6\n480 320\n255\n' + b''.join(
    open('%s/b_%d.ppm' % (d, i),'rb').read().split(b'\n',3)[3] for i in range(33))
print('  33 bands stitched:', 'IDENTICAL' if out == ref else 'DIFFERENT')
sys.exit(0 if out == ref else 1)
" "$SCRATCH" || die "the bands do not stitch -- the split is not exact"
}

# -------------------------------------------------------------------------------------------
case "${1:-all}" in
    build)   cmd_build ;;
    smoke)   cmd_smoke ;;
    dist)    cmd_dist ;;
    bench)   cmd_bench ;;
    quality) cmd_quality ;;
    plan)    cmd_plan ;;
    boards)  cmd_boards ;;
    graphics) cmd_graphics ;;
    push)    cmd_push "$@" ;;
    all)     cmd_build && cmd_smoke && cmd_dist ;;
    *)       sed -n '2,25p' "${BASH_SOURCE[0]}" ;;
esac
