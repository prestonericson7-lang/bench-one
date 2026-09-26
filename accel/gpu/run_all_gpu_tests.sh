#!/bin/bash
# run_all_gpu_tests.sh -- every automated test of the FPGA-GPU, from its repo location (WSL, root):
#   RTL I/O-block testbenches, daemon (x86 simulator + ARM binary under qemu), geometry/reference
#   tests, and the Pi-side end-to-end run against the simulated PL + simulated Teensy.
set -u
G=$(cd "$(dirname "$0")" && pwd); cd "$G"
fail=0; res=()
step() { local n=$1; shift; echo; echo "=== $n"; if "$@"; then res+=("PASS  $n"); else res+=("FAIL  $n"); fail=1; fi; }
step "RTL: Verilator lint of the whole GPU incl. gpu_pl + header/port checks" bash sim/lint.sh
step "RTL: render core vs the golden model, every scene" bash sim/run_core_scenes.sh
step "RTL: I/O blocks (par_rx, fifos, axi_gp_regs, tmds)" bash sim/run_io.sh
step "RTL: scanout"                                      bash sim/run_scanout.sh
step "daemon: build (x86 sim + static ARM), no warnings"  make -C zynq WERROR=-Werror sim arm
step "daemon: tests vs fpgagpud --sim"                   make -C zynq test
step "daemon: tests vs ARM binary under qemu-arm"        make -C zynq test-arm
step "geometry + reference renderer tests"               make -C tests
step "Pi tools end to end (sim PL + sim Teensy)"         bash pi/sim_e2e.sh
echo; printf '%s\n' "${res[@]}"
[ $fail = 0 ] && echo "GPU TESTS: ALL PASS" || echo "GPU TESTS: FAILURES"
exit $fail
