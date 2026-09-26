`timescale 1ns / 1ps
// tb_sync_fifo -- random push/pop test of rtl/sync_fifo.v against a queue model.
//
// Two instances run in parallel: a small FIFO (W=8, AW=4: full/empty corners every few cycles)
// and the Teensy FIFO configuration (W=33, AW=10). Traffic phases: balanced, fill-heavy,
// drain-heavy, write-only (sits at full with writes being dropped), read-only (sits at empty
// with pops being ignored), both-always; plus a mid-run reset.
// Checked every cycle: dout == model head whenever !empty, count/free/full exact, empty never
// hides data for more than 1 cycle (FWFT latency), no data loss/duplication/reordering.
// Plusargs: +seed=N  +cycles=N
// Prints "PASS tb_sync_fifo" or "FAIL tb_sync_fifo".
module tb_sync_fifo;
    reg clk = 1'b0;
    always #3.361 clk = ~clk;              // 148.75 MHz core clock (6.722 ns)

    integer seed   = 1;
    integer cycles = 200000;
    wire    done_s, done_l;
    wire [31:0] err_s, err_l;

    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        if ($value$plusargs("cycles=%d", cycles)) ;
    end

    fifo_check #(.W(8),  .AW(4),  .ID(0)) u_small (.clk(clk), .seed_in(seed),         .cycles(cycles), .done(done_s), .errors(err_s));
    fifo_check #(.W(33), .AW(10), .ID(1)) u_large (.clk(clk), .seed_in(seed + 12345), .cycles(cycles), .done(done_l), .errors(err_l));

    initial begin
        wait (done_s && done_l);
        #20;
        if (err_s == 0 && err_l == 0)
            $display("PASS tb_sync_fifo (seed %0d, %0d cycles per instance)", seed, cycles);
        else
            $display("FAIL tb_sync_fifo (errors: small %0d, large %0d)", err_s, err_l);
        $finish;
    end
endmodule

module fifo_check #(
    parameter W  = 8,
    parameter AW = 4,
    parameter ID = 0
) (
    input  wire        clk,
    input  wire [31:0] seed_in,
    input  wire [31:0] cycles,
    output reg         done,
    output reg [31:0]  errors
);
    localparam DEPTH = 1 << AW;

    reg          rst = 1'b1;
    reg          wr = 1'b0, rd = 1'b0;
    reg  [W-1:0] din = {W{1'b0}};
    wire [W-1:0] dout;
    wire         empty, full;
    wire [AW:0]  count, free;

    sync_fifo #(.W(W), .AW(AW)) dut (
        .clk(clk), .rst(rst), .wr(wr), .din(din), .rd(rd),
        .dout(dout), .empty(empty), .full(full), .count(count), .free(free)
    );

    // queue model
    reg [W-1:0] q [0:DEPTH-1];
    integer qh, qt, qn;
    integer seed;
    integer cyc;
    integer phase_mode;
    integer pw, pr;                 // push / pop probability in percent
    integer empty_run;
    integer n_push, n_pop, n_drop_full, n_ign_empty, n_full_cyc, n_empty_cyc;

    initial begin
        done = 1'b0; errors = 0;
        qh = 0; qt = 0; qn = 0; cyc = 0; empty_run = 0;
        n_push = 0; n_pop = 0; n_drop_full = 0; n_ign_empty = 0; n_full_cyc = 0; n_empty_cyc = 0;
        #1 seed = seed_in;
    end

    task err;
        input [8*80-1:0] msg;
        begin
            errors = errors + 1;
            if (errors <= 10)
                $display("ERROR fifo_check[%0d] cycle %0d: %0s (count=%0d model=%0d empty=%b full=%b)",
                         ID, cyc, msg, count, qn, empty, full);
        end
    endtask

    // ---- stimulus: change inputs on the falling edge ----
    always @(negedge clk) begin
        if (!done) begin
            phase_mode = (cyc / 1500) % 6;
            case (phase_mode)
                0: begin pw = 50;  pr = 50;  end    // balanced
                1: begin pw = 80;  pr = 25;  end    // fill
                2: begin pw = 25;  pr = 80;  end    // drain
                3: begin pw = 100; pr = 0;   end    // sit at full, writes dropped
                4: begin pw = 0;   pr = 100; end    // sit at empty, pops ignored
                default: begin pw = 100; pr = 100; end
            endcase
            wr  <= (($random(seed) & 32'h7fffffff) % 100) < pw;
            rd  <= (($random(seed) & 32'h7fffffff) % 100) < pr;
            din <= {$random(seed), $random(seed)};
            // reset for 3 cycles at 5 and at ~60 % of the run
            rst <= (cyc < 5) || (cyc >= (cycles * 3) / 5 && cyc < (cycles * 3) / 5 + 3);
        end
    end

    // ---- checks + model update on the rising edge (DUT outputs are pre-edge values) ----
    always @(posedge clk) begin
        if (!done) begin
            if (rst) begin
                qh = 0; qt = 0; qn = 0; empty_run = 0;
            end else begin
                // output checks
                if (count !== qn[AW:0])               err("count mismatch");
                if (free !== (DEPTH - qn))             err("free mismatch");
                if (full !== (qn == DEPTH))            err("full mismatch");
                if (!empty && qn == 0)                 err("not empty but model empty");
                if (!empty && dout !== q[qh])          err("dout != model head");
                if (empty && qn > 1)                   err("empty while >1 entries stored");
                if (empty && qn > 0) empty_run = empty_run + 1; else empty_run = 0;
                if (empty_run > 1)                     err("data hidden for >1 cycle (FWFT latency)");
                if (full)  n_full_cyc  = n_full_cyc + 1;
                if (empty) n_empty_cyc = n_empty_cyc + 1;
                // model update (same acceptance rules as the DUT, on pre-edge flags)
                if (rd) begin
                    if (!empty) begin
                        qh = (qh + 1) % DEPTH; qn = qn - 1; n_pop = n_pop + 1;
                    end else n_ign_empty = n_ign_empty + 1;
                end
                if (wr) begin
                    if (!full) begin
                        q[qt] = din; qt = (qt + 1) % DEPTH; qn = qn + 1; n_push = n_push + 1;
                    end else n_drop_full = n_drop_full + 1;
                end
            end
            cyc = cyc + 1;
            if (cyc >= cycles) begin
                $display("fifo_check[%0d] W=%0d AW=%0d: %0d pushes, %0d pops, %0d dropped-when-full, %0d ignored-when-empty, %0d full cycles, %0d empty cycles, %0d errors",
                         ID, W, AW, n_push, n_pop, n_drop_full, n_ign_empty, n_full_cyc, n_empty_cyc, errors);
                if (n_drop_full == 0 || n_ign_empty == 0 || n_full_cyc == 0) begin
                    errors = errors + 1;
                    $display("ERROR fifo_check[%0d]: coverage hole (full/empty corners not reached)", ID);
                end
                done = 1'b1;
            end
        end
    end
endmodule
