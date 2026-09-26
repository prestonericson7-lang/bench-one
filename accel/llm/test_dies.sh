#!/bin/bash
# test_dies.sh -- the Zynq goes away in the middle of a generation: the runtime must recompute the
# lost rows on the CPU, switch the offload off, and still produce the whole answer. WSL.
set -u
H=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-/mnt/d/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba}
[ -f "$MODEL" ] || { echo "SKIP: no model"; exit 0; }
PORT=18199; OUT=/tmp/zdies.txt
"$H/../zynq/out/zaccel-server-x86" --cpu --cpu-mb 2400 --port $PORT >/tmp/zdies_srv.log 2>&1 & SV=$!; sleep 1
"$H/out/run_model" "$MODEL" "def fibonacci(n):" 40 --fast --zaccel 127.0.0.1:$PORT --share 0.5 >$OUT 2>&1 & RM=$!
# wait until it is generating (the prefill has run through the Zynq), then pull the Zynq away
for i in $(seq 1 600); do grep -q "prefill" $OUT && break; sleep 1; done
sleep 3; kill -9 $SV; echo "zaccel-server killed while generating"
wait $RM; rc=$?
grep -E "zaccel:|offload off" $OUT | sed 's/^/  /'
sed -n '/^  ----/,/^  ----/p' $OUT > /tmp/zdies_text.txt
ok=1
[ $rc = 0 ] || { echo "FAIL run_model exit $rc"; ok=0; }
grep -q "offload off" $OUT || { echo "FAIL no fallback message"; ok=0; }
lines=$(grep -c . /tmp/zdies_text.txt)
[ "$lines" -ge 6 ] || { echo "FAIL the answer is incomplete ($lines lines)"; ok=0; }
cat /tmp/zdies_text.txt
[ $ok = 1 ] && echo "PASS: the Zynq died mid-generation and the answer still came out whole" && exit 0
exit 1
