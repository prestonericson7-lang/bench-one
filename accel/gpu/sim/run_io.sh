#!/bin/bash
# sim/run_io.sh -- Icarus Verilog regression for the PL I/O modules (engineer D):
#   tb_sync_fifo    rtl/sync_fifo.v                 random push/pop vs model, 2 configurations
#   tb_par_rx       rtl/par_rx.v + sync_fifo        Teensy bus model, real-time setup/hold,
#                                                   STROBE glitch injection (phase I)
#   tb_axi_gp_regs  rtl/axi_gp_regs.v + sync_fifo   AXI3 master BFM, registers, PS FIFO push
#   tb_clkgen       rtl/clkgen.v + MMCM/BUFG stubs  (testbench generated below) output clock
#                   frequencies 148.75 / 74.375 / 371.875 MHz, clk_ser edges aligned with
#                   clk_pix edges, per-domain reset release after LOCKED
# All testbenches run the core clock at 148.75 MHz. Each test runs with several seeds
# (tb_clkgen is deterministic: one run). Usage:  bash sim/run_io.sh [seed ...]
# Exit status 0 only if every run prints its PASS line and no FAIL/ERROR line.
set -u
cd "$(dirname "$0")/.." || exit 2

SEEDS=("$@")
[ ${#SEEDS[@]} -eq 0 ] && SEEDS=(1 7 12345)

WORK=$(mktemp -d /tmp/run_io.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
IV="iverilog -g2001 -Wall -Irtl"

pass=0
fail=0

run_tb() {   # name "extra plusargs" files...
    local name=$1; local args=$2; shift 2
    echo "== $name: compile"
    if ! $IV -s "$name" -o "$WORK/$name.vvp" "$@" > "$WORK/$name.comp.log" 2>&1; then
        cat "$WORK/$name.comp.log"
        echo "   COMPILE FAIL"
        fail=$((fail + 1))
        return
    fi
    # iverilog warnings are shown but do not fail the run. The RTL has no `timescale (team
    # convention) and inherits the testbench's; those notes are filtered out.
    grep -v "^\s*$" "$WORK/$name.comp.log" | grep -v "timescale" | sed 's/^/   [iverilog] /'
    for s in "${SEEDS[@]}"; do
        # shellcheck disable=SC2086
        vvp -n "$WORK/$name.vvp" +seed=$s $args > "$WORK/$name.$s.log" 2>&1
        if grep -q "^PASS $name" "$WORK/$name.$s.log" && ! grep -q "FAIL\|ERROR" "$WORK/$name.$s.log"; then
            echo "   seed $s: $(grep "^PASS $name" "$WORK/$name.$s.log")"
            grep "^INFO" "$WORK/$name.$s.log" | sed 's/^/      /'
            pass=$((pass + 1))
        else
            echo "   seed $s: FAIL -- log tail:"
            grep -m 30 "ERROR\|FAIL" "$WORK/$name.$s.log" | sed 's/^/      /'
            tail -n 5 "$WORK/$name.$s.log" | sed 's/^/      /'
            fail=$((fail + 1))
        fi
    done
}

run_tb tb_sync_fifo   ""  sim/tb_sync_fifo.v rtl/sync_fifo.v
run_tb tb_par_rx      ""  sim/tb_par_rx.v rtl/par_rx.v rtl/sync_fifo.v
run_tb tb_axi_gp_regs ""  sim/tb_axi_gp_regs.v rtl/axi_gp_regs.v rtl/sync_fifo.v

# ---- clkgen (deterministic, one run) -----------------------------------------------------------
cat > "$WORK/tb_clkgen.v" <<'EOF'
`timescale 1ns / 1ps
module tb_clkgen;
    reg clk50 = 1'b0;
    always #10 clk50 = ~clk50;                       // 50 MHz board oscillator
    wire clk, clk_pix, clk_ser, locked, rst, rst_pix;
    clkgen dut (.clk50(clk50), .clk(clk), .clk_pix(clk_pix), .clk_ser(clk_ser),
                .locked(locked), .rst(rst), .rst_pix(rst_pix));

    integer errors = 0;
    task err(input [8*80-1:0] m); begin errors = errors + 1; $display("ERROR %0s", m); end endtask

    reg meas = 1'b0;
    integer nc = 0, np = 0, ns = 0, nmis = 0;
    realtime tc0 = 0, tc1 = 0, tp0 = 0, tp1 = 0, ts0 = 0, ts1 = 0, last_ser = -1.0;
    always @(posedge clk)     if (meas) begin if (nc == 0) tc0 = $realtime; nc = nc + 1; tc1 = $realtime; end
    always @(posedge clk_pix) if (meas) begin if (np == 0) tp0 = $realtime; np = np + 1; tp1 = $realtime; end
    always @(posedge clk_ser) begin
        last_ser = $realtime;
        if (meas) begin if (ns == 0) ts0 = $realtime; ns = ns + 1; ts1 = $realtime; end
    end
    // OSERDESE2 needs CLK (clk_ser) and CLKDIV (clk_pix) phase-aligned: a clk_ser rising edge at
    // every clk_pix rising edge
    always @(posedge clk_pix) if (meas) begin
        #0.001;
        if ($realtime - 0.001 - last_ser > 0.0001 || $realtime - 0.001 - last_ser < -0.0001)
            nmis = nmis + 1;
    end
    // resets must be high whenever LOCKED is low
    always @(posedge clk)     if (!locked && !rst)     err("rst low while MMCM not locked");
    always @(posedge clk_pix) if (!locked && !rst_pix) err("rst_pix low while MMCM not locked");

    real fc, fp, fs;
    integer k;
    initial begin
        if (rst !== 1'b1 || rst_pix !== 1'b1) err("resets not asserted at power-up");
        wait (locked === 1'b1);
        k = 0;
        while (rst === 1'b1 && k < 100) begin @(posedge clk); k = k + 1; end
        $display("INFO rst released %0d core clocks after LOCKED", k);
        if (k < 16 || k > 24) err("core reset release not 16..24 clocks after LOCKED");
        if (rst_pix !== 1'b0) @(negedge rst_pix);
        meas = 1'b1;
        #20000;
        meas = 1'b0;
        fc = 1000.0 * (nc - 1) / (tc1 - tc0);
        fp = 1000.0 * (np - 1) / (tp1 - tp0);
        fs = 1000.0 * (ns - 1) / (ts1 - ts0);
        $display("INFO clk %0.3f MHz, clk_pix %0.3f MHz, clk_ser %0.3f MHz (stub tick rounded to 1 ps)",
                 fc, fp, fs);
        if (fc < 148.75 * 0.999 || fc > 148.75 * 1.001) err("core clock not 148.75 MHz");
        if (fp < 74.375 * 0.999 || fp > 74.375 * 1.001) err("pixel clock not 74.375 MHz");
        if (fs < 371.875 * 0.999 || fs > 371.875 * 1.001) err("serial clock not 371.875 MHz");
        if (nmis != 0) err("clk_ser rising edge missing at a clk_pix rising edge");
        if (rst !== 1'b0 || rst_pix !== 1'b0) err("a reset re-asserted while locked");
        if (errors == 0) $display("PASS tb_clkgen (%0d clk_pix edges phase-checked)", np);
        else             $display("FAIL tb_clkgen (%0d errors)", errors);
        $finish;
    end
endmodule
EOF
SEEDS_ALL=("${SEEDS[@]}")
SEEDS=(1)
run_tb tb_clkgen      ""  "$WORK/tb_clkgen.v" rtl/clkgen.v rtl/sim_stubs/MMCME2_ADV.v rtl/sim_stubs/BUFG.v
SEEDS=("${SEEDS_ALL[@]}")

echo
echo "run_io: $pass passed, $fail failed"
if [ $fail -eq 0 ]; then echo "RUN_IO PASS"; exit 0; else echo "RUN_IO FAIL"; exit 1; fi
