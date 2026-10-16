#!/bin/bash
# test_two_engines.sh -- two engines must give the SAME BYTES as one (WSL, correctness only).
#   Every row of a matrix is computed whole by exactly one engine, so splitting the engines' rows
#   between two servers changes which server computes a row and nothing else. The generated text and
#   the perplexity with two engines must therefore equal those with one engine, exactly.
#   Also: one of the two servers killed mid-run -> the other carries on and the text is still complete.
# Needs a GGUF model: $MODEL (default the local Ollama qwen2.5-coder:3b blob). Absent -> SKIP (exit 0).
set -u
H=$(cd "$(dirname "$0")" && pwd)
export MODEL=${MODEL:-/mnt/d/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba}
[ -f "$MODEL" ] || { echo "SKIP: no model at $MODEL"; exit 0; }
bash "$H/build_host.sh" >/tmp/llm_build2.log 2>&1 || { tail -5 /tmp/llm_build2.log; echo "FAIL build"; exit 1; }
RM="$H/out/run_model"; PPL="$H/out/ppl"; SRV="$H/../zynq/out/zaccel-server-x86"    # build_host.sh's native binaries
[ -x "$RM" ] && [ -x "$PPL" ] && [ -x "$SRV" ] || { echo "FAIL: need $RM, $PPL and $SRV (build_host.sh, accel/zynq/build.sh)"; exit 1; }
P="def fibonacci(n):"; N=24
# the generated text is the block between run_model's two dashed rules; everything else is timing
gen() { awk '/^  -{20,}/ { n++; next } n == 1'; }
"$SRV" --cpu --cpu-mb 3000 --port 18201 >/tmp/two_a.log 2>&1 & A=$!
"$SRV" --cpu --cpu-mb 3000 --port 18202 >/tmp/two_b.log 2>&1 & B=$!
sleep 1
rc=0
one=$("$RM" "$MODEL" "$P" $N --fast --zaccel 127.0.0.1:18201 --share 0.5 2>/dev/null | gen)
two=$("$RM" "$MODEL" "$P" $N --fast --zaccel 127.0.0.1:18201,127.0.0.1:18202 --share 0.5 2>/dev/null | gen)
[ -n "$one" ] && [ "$one" = "$two" ] && echo "PASS text: two engines == one engine, byte for byte ($(echo "$one" | wc -c) bytes)" || { echo "FAIL text differs (or empty)"; echo "--- one:"; echo "$one" | head -5; echo "--- two:"; echo "$two" | head -5; rc=1; }
p1=$("$PPL" "$MODEL" --fast --limit 64 --zaccel 127.0.0.1:18201 --share 0.5 2>/dev/null | awk '/PERPLEXITY/ {print $2}')
p2=$("$PPL" "$MODEL" --fast --limit 64 --zaccel 127.0.0.1:18201,127.0.0.1:18202 --share 0.5 2>/dev/null | awk '/PERPLEXITY/ {print $2}')
[ -n "$p1" ] && [ "$p1" = "$p2" ] && echo "PASS perplexity: $p1 with one engine == $p2 with two" || { echo "FAIL perplexity: one=$p1 two=$p2"; rc=1; }
# one engine dies mid-run: a run is ~45 s here (about 10 s of load + upload, then the tokens), so B is
# killed at 25 s, during generation; the run must complete, print text, and name the retired engine
( sleep 25; kill $B 2>/dev/null ) &
died=$("$RM" "$MODEL" "$P" $N --fast --zaccel 127.0.0.1:18201,127.0.0.1:18202 --share 0.5 2>/tmp/two_died.err)
grep -q "retired" /tmp/two_died.err && echo "PASS retire: $(grep -m1 retired /tmp/two_died.err | cut -c1-110)" || { echo "FAIL retire: engine B's death at 25 s was not reported"; rc=1; }
# After the loss the dead engine's rows come from the CPU's own GGUF math, not the int8 copy, so the
# text may legitimately differ from the two-engine text; what must hold is that an answer was produced.
txt=$(echo "$died" | gen)
[ -n "$txt" ] && echo "PASS complete: an answer after the loss ($(echo "$txt" | wc -c) bytes)" || { echo "FAIL: no text after an engine died"; rc=1; }
kill $A $B 2>/dev/null; wait 2>/dev/null
[ $rc = 0 ] && echo "TWO ENGINES: PASS" || echo "TWO ENGINES: FAIL"
exit $rc
