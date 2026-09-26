#!/bin/bash
# bench_day.sh -- on the Orange Pi, with the Zynq up: every measurement that says how much the Zynq +
# Teensy add, in one run, saved to a report. These are the numbers that count (a PC's never do).
#   bench_day.sh [model.gguf]            (after install_pi.sh; with no model it uses the one the card
#                                         was built with, /opt/accel/models/*.gguf, if there is one)
# Report: ~/accel-bench-<date>.txt
set -u
MODEL=${1:-$(ls /opt/accel/models/*.gguf 2>/dev/null | head -1)}
OUT=~/accel-bench-$(date +%Y%m%d-%H%M%S).txt
exec > >(tee "$OUT") 2>&1
say() { printf '\n==== %s\n' "$*"; }

say "this board"
uname -a; lscpu 2>/dev/null | grep -E "Model name|^CPU\(s\)|MHz" ; free -m | head -2
say "the Zynq"
for h in 10.20.0.2 10.77.0.2; do ping -c3 -W1 "$h" 2>/dev/null | tail -1 | sed "s/^/$h: /"; done
timeout 20 python3 /usr/local/lib/zaccel/zaccel.py 2>&1 | head -8

say "matrix engine: Pi alone / Zynq alone / both (every answer checked)"
timeout 1800 zaccel-bench 2>&1 | tail -n 60

say "GPU: bit-exact self test + frames per second"
timeout 900 gpu_selftest 2>&1 | tail -n 40

say "Zynq RAM as swap"
swapon --show; systemctl --no-pager --lines=0 status zaccel-swap.service 2>&1 | head -4
if swapon --show=NAME --noheadings | grep -q nbd; then
  dev=$(swapon --show=NAME --noheadings | grep nbd | head -1)
  echo "read throughput of $dev (O_DIRECT, 64 MB):"
  timeout 60 dd if="$dev" of=/dev/null bs=1M count=64 iflag=direct 2>&1 | tail -1
fi

if [ -n "$MODEL" ] && [ -f "$MODEL" ]; then
  say "a real model: Pi alone"
  timeout 3600 run_model "$MODEL" "def fibonacci(n):" 64 --fast 2>&1 | grep -E "prefill|decode|per decoded|loaded"
  say "a real model: Pi + Zynq (split measured at start)"
  timeout 3600 run_model "$MODEL" "def fibonacci(n):" 64 --fast --zaccel auto 2>&1 | grep -E "zaccel|prefill|decode|per decoded"
  say "quality: perplexity, Pi alone vs Pi + Zynq"
  timeout 7200 ppl "$MODEL" --fast --limit 256 2>&1 | grep -E "PERPLEXITY|top-1"
  timeout 7200 ppl "$MODEL" --fast --limit 256 --zaccel auto 2>&1 | grep -E "zaccel|PERPLEXITY|top-1"
else
  say "model runs skipped (give a .gguf path to include them)"
fi
say "report saved: $OUT"
