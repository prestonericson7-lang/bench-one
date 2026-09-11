#!/bin/sh
# ===========================================================================================
#  run_host_tests.sh -- build and RUN every self-verifying test in this directory
# ===========================================================================================
#
#  Compiling proves almost nothing. Until this script existed there was no way to run any of these
#  at all: each one calls into shared/ and none of them link on their own, so "it compiles" was the
#  only check anybody had been doing. The first run of this script found a failing claim that had
#  been failing for as long as the test had existed.
#
#  WHAT THIS IS AND IS NOT FOR
#  ---------------------------
#  CORRECTNESS ONLY. Nothing here reports a speed. This machine is built out of microcontrollers and
#  small SBCs, and a desktop shares none of their constraints, so a desktop timing is not evidence
#  about the machine and is not printed. What a desktop CAN settle is whether an algorithm returns
#  the right answer, and that is the same answer everywhere.
#
#  The benchmarks in this directory -- decode_limit, stage_bench, gemv_host, hop_bench and the rest --
#  are deliberately NOT run here. They belong on a board.
#
#  WHAT EACH TEST CHECKS
#  ---------------------
#   born_test       the hyperdimensional frame: that magnitude is smooth and monotonic, that
#                   nearby positions are nearby vectors, and that a moment can be unrotated back
#                   to the thing it was a moment of
#   wonder_test     that the machine holds possibilities and forms its own questions, and that a
#                   fact about itself lands in its own region
#   system_test     the distributed store: that merging is a monoid under reordering, duplication
#                   and retry, that a vector is found if and only if some node actually scanned it,
#                   and that reported coverage never exceeds what was really examined
#   deep_test       the second recall stage: that a complete deep pass reproduces an exhaustive
#                   full-width search exactly, that it is a monoid, that an INCOMPLETE pass is
#                   refused rather than reported, and -- the awkward one -- that under the row
#                   sharding this project actually builds it cannot change the answer at all
#   dot_verify      that the fused quantized dot product agrees with the scalar reference BIT FOR
#                   BIT on every row of a real tensor. Prints rates; ignore them, they are a host.
#
#  A TRAP IN dot_verify WORTH KNOWING ABOUT
#  ----------------------------------------
#  Its vector kernel is behind `#if defined(__AVX2__)`. Built without -mavx2 the "vector" path IS the
#  scalar path, so the test compares a function against itself, prints IDENTICAL on all 4096 rows, and
#  has verified nothing at all. It reports a 1.00x speedup when that happens, which is the only visible
#  sign.
#
#  So this script probes for AVX2 and NEON and turns them on when the host has them, then prints which
#  kernel was actually compared. If it says "scalar", the bit-identity result is vacuous on this
#  machine -- that is not a failure, some hosts genuinely have neither, but it is not evidence either.
#
#  RUN
#      sh run_host_tests.sh
#
#  Needs a C compiler on PATH. On Windows, w64devkit's gcc works and is what this was verified with;
#  the -lws2_32 below is for it, since shared/stage_link.c uses sockets and MinGW does not link
#  winsock by default. On Linux that flag is silently unnecessary and is dropped automatically.
# ===========================================================================================
set -e
D="$(cd "$(dirname "$0")" && pwd)"
B="$D/../shared"
OUT="$D/.hosttest"
mkdir -p "$OUT/obj"

CC="${CC:-gcc}"
command -v "$CC" >/dev/null 2>&1 || { echo "no C compiler: set CC or put gcc on PATH"; exit 2; }

# Winsock only exists on Windows, and naming it on Linux is an error rather than a no-op.
SOCKLIB=""
case "$(uname -s 2>/dev/null || echo Windows)" in
  *MINGW*|*MSYS*|*CYGWIN*|Windows*) SOCKLIB="-lws2_32" ;;
esac

# Probe for a vector unit, so dot_verify actually compares two different kernels. Compiling a tiny
# program is the only reliable test: the compiler knows what it can emit and uname does not.
SIMD=""
SIMDNAME="scalar"
probe() {
  echo 'int main(void){return 0;}' > "$OUT/probe.c"
  if "$CC" $1 -o "$OUT/probe" "$OUT/probe.c" >/dev/null 2>&1; then return 0; fi
  return 1
}
if probe "-mavx2 -mfma"; then
  SIMD="-mavx2 -mfma"; SIMDNAME="AVX2"
elif probe "-mfpu=neon"; then
  SIMD="-mfpu=neon"; SIMDNAME="NEON"
fi
rm -f "$OUT/probe.c" "$OUT/probe" "$OUT/probe.exe"

echo "=== building shared/  (vector unit: $SIMDNAME) ==="
n=0
for f in "$B"/*.c; do
  "$CC" -std=c11 -O2 $SIMD -I"$B" -c -o "$OUT/obj/$(basename "$f" .c).o" "$f"
  n=$((n + 1))
done
echo "  $n objects"
if [ "$SIMDNAME" = "scalar" ]; then
  echo "  NOTE: no vector unit available, so dot_verify will compare the scalar kernel"
  echo "        against itself. Its bit-identity result proves nothing on this host."
fi

# Every test links against ALL of shared/. Picking per-test dependency lists was tried and is a
# maintenance trap: a new call in a test silently fails to link, which reads as the test being
# broken. The linker drops what is unused.
TESTS="born_test wonder_test system_test deep_test dot_verify"

echo
echo "=== running ==="
fail=0
for t in $TESTS; do
  if ! "$CC" -std=c11 -O2 $SIMD -I"$B" -I"$D" -o "$OUT/$t" "$D/$t.c" "$OUT"/obj/*.o -lm $SOCKLIB 2>"$OUT/$t.build"; then
    echo "  BUILD FAIL  $t"
    sed 's/^/      /' "$OUT/$t.build" | head -8
    fail=1
    continue
  fi
  if "$OUT/$t" > "$OUT/$t.log" 2>&1; then
    note=""
    if [ "$t" = dot_verify ]; then
      note=" (compared $(sed -n 's/.*this build uses: //p' "$OUT/$t.log" | head -1) against scalar)"
    fi
    printf '  pass  %-14s %s assertions%s\n' "$t" \
      "$(grep -ciE 'yes|IDENTICAL|hold' "$OUT/$t.log" 2>/dev/null || echo 0)" "$note"
  else
    echo "  FAIL  $t"
    grep -E "FAIL|CLAIM|WRONG" "$OUT/$t.log" | head -10 | sed 's/^/      /'
    fail=1
  fi
done

echo
if [ "$fail" -ne 0 ]; then
  echo "=== SOMETHING FAILED. Full output is in $OUT/ ==="
  exit 1
fi
echo "=== all host tests pass. Full output in $OUT/ ==="
echo "    These are correctness results. For speed, run the benchmarks on a board."
