#!/bin/bash
# test_generate.sh -- the model writes the same prompt with every row on this CPU, then with half of
# every dense matrix's rows on the Zynq path (decode AND prefill offloaded). The "Zynq" is zaccel-server
# --cpu on this machine: integer results identical to the PL. Output text is shown side by side; the
# yardstick for the approximation is test_quality.sh (perplexity), this shows it in use. WSL.
set -u
H=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-$H/out/run_model}
MODEL=${MODEL:-/mnt/d/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba}
PROMPT=${PROMPT:-def fibonacci(n):}
PORT=18194
"$H/../zynq/out/zaccel-server-x86" --cpu --cpu-mb 2400 --port $PORT >/tmp/zg_server.log 2>&1 &
S=$!; sleep 1
gen() { "$BIN" "$MODEL" "$PROMPT" 40 --fast "$@" 2>&1 | sed -n '/^  ----/,/^  ----/p'; }
echo "== every row on this CPU";                 gen | tee /tmp/zg_base.txt
echo "== half of every matrix's rows on the Zynq path"
"$BIN" "$MODEL" "$PROMPT" 40 --fast --zaccel 127.0.0.1:$PORT --share 0.5 2>&1 | grep -E "zaccel:"
gen --zaccel 127.0.0.1:$PORT --share 0.5 | tee /tmp/zg_off.txt
cmp -s /tmp/zg_base.txt /tmp/zg_off.txt && echo "RESULT: identical text" || echo "RESULT: text differs (an approximation: judge it by test_quality.sh)"
kill $S 2>/dev/null
