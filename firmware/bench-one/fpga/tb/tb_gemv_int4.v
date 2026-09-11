/* ===========================================================================================
 *  tb_gemv_int4.v -- does the fabric compute the same dot product the C does?
 * ===========================================================================================
 *  The core runs at exactly the right speed whether or not the nibble order is right, so it is
 *  checked against an independently computed expected value rather than against itself. Ragged
 *  rows, back-pressure and stream overrun are directed cases because each has a silent failure.
 *
 *  RUN
 *      iverilog -g2005 -o tb.vvp ../rtl/gemv_int4.v tb_gemv_int4.v && vvp tb.vvp
 * ===========================================================================================
 */

`default_nettype none
`timescale 1ns / 1ps

module tb_gemv_int4;

    localparam integer LANES = 8;
    localparam integer MAXC  = 512;

    reg                    clk = 1'b0, rst_n = 1'b0;
    reg                    a_wr = 1'b0;
    reg  [15:0]            a_addr = 16'd0;
    reg  [(LANES*8)-1:0]   a_data = {(LANES*8){1'b0}};
    reg                    start = 1'b0;
    reg  [15:0]            cols = 16'd0;
    reg                    w_valid = 1'b0;
    reg  [(LANES*4)-1:0]   w_data = {(LANES*4){1'b0}};
    wire                   w_ready, done;
    wire signed [31:0]     acc;
    wire [15:0]            consumed;

    gemv_int4 #(.LANES(LANES)) dut (
        .clk(clk), .rst_n(rst_n),
        .a_wr(a_wr), .a_addr(a_addr), .a_data(a_data),
        .start(start), .cols(cols),
        .w_valid(w_valid), .w_data(w_data), .w_ready(w_ready),
        .done(done), .acc(acc), .consumed(consumed)
    );

    always #5 clk = ~clk;

    integer errors = 0;
    integer i, c, seed;

    reg signed [7:0] act  [0:MAXC-1];
    reg signed [3:0] wgt  [0:MAXC-1];

    reg done_seen = 1'b0;
    always @(posedge clk) if (done) done_seen <= 1'b1;

    /* Expected value, accumulated one product at a time so it shares no structure with the core. */
    function signed [31:0] expected(input integer n);
        integer k;
        reg signed [31:0] s;
        begin
            s = 32'sd0;
            for (k = 0; k < n; k = k + 1) s = s + (wgt[k] * act[k]);
            expected = s;
        end
    endfunction

    /* Activations go in LANES at a time now, which is how they arrive from the previous pipeline
     * stage anyway. */
    task load_activations(input integer n);
        integer L;
        begin
            for (i = 0; i < n; i = i + LANES) begin
                @(negedge clk);
                a_wr   = 1'b1;
                a_addr = (i / LANES);
                for (L = 0; L < LANES; L = L + 1)
                    a_data[(L*8) +: 8] = ((i + L) < n) ? act[i + L] : 8'sd0;
            end
            @(negedge clk);
            a_wr = 1'b0;
        end
    endtask

    /* Feed the row, with a gap every third beat so the ready/valid handshake is exercised rather
     * than assumed, and `extra` beats of junk afterwards to prove the core stops accepting. */
    task feed_row(input integer n, input integer extra, output integer junk_taken);
        integer sent, gap, guard, L;
        begin
            sent = 0; gap = 0; guard = 0; junk_taken = 0;
            while (sent < n && guard < 10000) begin
                @(negedge clk);
                if (gap == 2) begin
                    w_valid = 1'b0;
                    gap = 0;
                end
                else begin
                    w_valid = 1'b1;
                    /* Pad with -8, not zero.
                     *
                     * Zero padding is invisible: a lane outside the row contributes zero whether or
                     * not the core masks it, so a mutation that counts the padding passes every
                     * test. A DMA streaming from memory does not pad with zeros either, it delivers
                     * whatever bytes come next, so -8 is both the harsher test and the truer one. */
                    for (L = 0; L < LANES; L = L + 1)
                        w_data[(L*4) +: 4] = ((sent + L) < n) ? wgt[sent + L] : -4'sd8;
                    gap = gap + 1;
                end
                @(posedge clk);
                if (w_valid && w_ready) sent = sent + LANES;
                guard = guard + 1;
            end
            /* Keep offering beats the row does not contain. */
            for (guard = 0; guard < extra; guard = guard + 1) begin
                @(negedge clk);
                w_valid = 1'b1;
                for (L = 0; L < LANES; L = L + 1) w_data[(L*4) +: 4] = 4'sd7;
                @(posedge clk);
                if (w_ready) junk_taken = junk_taken + 1;
            end
            @(negedge clk);
            w_valid = 1'b0;
        end
    endtask

    task run_case(input [255:0] name, input integer n, input integer extra);
        integer junk;
        reg signed [31:0] want;
        begin
            done_seen = 1'b0;
            @(negedge clk);
            cols  = n[15:0];
            start = 1'b1;
            @(negedge clk);
            start = 1'b0;

            feed_row(n, extra, junk);

            i = 0;
            while (!done_seen && i < 500) begin @(posedge clk); i = i + 1; end
            @(negedge clk);

            want = expected(n);
            if (!done_seen) begin
                $display("  FAIL  %0s: never finished", name);
                errors = errors + 1;
            end
            else if (acc !== want) begin
                $display("  FAIL  %0s: acc %0d, expected %0d", name, acc, want);
                errors = errors + 1;
            end
            else if (consumed !== n[15:0]) begin
                $display("  FAIL  %0s: consumed %0d, expected %0d", name, consumed, n);
                errors = errors + 1;
            end
            else if (junk != 0) begin
                $display("  FAIL  %0s: accepted %0d beats past the end of the row", name, junk);
                errors = errors + 1;
            end
            else begin
                $display("  %-28s cols=%0d  acc=%0d  ok", name, n, acc);
            end
        end
    endtask

    initial begin
        $display("");
        $display("===============================================================");
        $display("gemv_int4 -- %0d lanes, INT4 weights x INT8 activations", LANES);
        $display("===============================================================");
        $display("");

        seed = 32'h1234_5678;
        for (i = 0; i < MAXC; i = i + 1) begin
            act[i] = $random(seed);
            wgt[i] = $random(seed);
        end

        repeat (4) @(negedge clk);
        rst_n = 1'b1;
        repeat (2) @(negedge clk);

        load_activations(MAXC);

        /* A clean multiple of LANES. */
        run_case("aligned row", 256, 0);

        /* Not a multiple of LANES: the last beat is ragged and its padding must not be counted. */
        run_case("ragged row", 250, 0);

        /* One beat exactly. */
        run_case("single beat", LANES, 0);

        /* Shorter than one beat: most of the first beat is padding. */
        run_case("shorter than one beat", 3, 0);

        /* The stream keeps running past the end of the row. */
        run_case("stream overruns the row", 128, 6);

        /* WORST CASE MAGNITUDE, which is what sizes the accumulator.
         *
         * Random weights and activations produce a random walk whose sum stays small -- every case
         * above landed inside 16 bits, so a 16-bit accumulator passed all of them. Driving every
         * weight and activation to its extreme makes the sum 7 x 127 x 512 = 455,168, which needs
         * 20 bits, and turns a silent overflow into a failed test. */
        for (i = 0; i < MAXC; i = i + 1) begin
            act[i] = 8'sd127;
            wgt[i] = 4'sd7;
        end
        load_activations(MAXC);
        run_case("worst-case magnitude", MAXC, 0);

        $display("");
        $display("===============================================================");
        if (errors == 0) $display("ALL CHECKS PASSED");
        else             $display("%0d CHECK(S) FAILED", errors);
        $display("===============================================================");
        $display("");
        if (errors != 0) $fatal(1);
        $finish;
    end

endmodule

`default_nettype wire
