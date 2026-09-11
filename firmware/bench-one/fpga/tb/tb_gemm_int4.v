/* ===========================================================================================
 *  tb_gemm_int4.v -- does a whole layer come out right, and how close to flat out does it run?
 * ===========================================================================================
 *  Checks every row against an independently computed value, not just the last one, because a
 *  wrapper that loses or repeats a row still produces plausible numbers for the rows it keeps.
 *  Then reports duty cycle, which is the number that says whether the core is starved.
 *
 *  RUN
 *      iverilog -g2005 -o tbm.vvp ../rtl/gemv_int4.v ../rtl/gemm_int4.v tb_gemm_int4.v
 *      vvp tbm.vvp
 * ===========================================================================================
 */

`default_nettype none
`timescale 1ns / 1ps

module tb_gemm_int4;

    localparam integer LANES = 8;
    localparam integer ROWS  = 24;
    localparam integer COLS  = 128;

    reg                    clk = 1'b0, rst_n = 1'b0;
    reg                    a_wr = 1'b0;
    reg  [15:0]            a_addr = 16'd0;
    reg  [(LANES*8)-1:0]   a_data = {(LANES*8){1'b0}};
    reg                    start = 1'b0;
    reg  [15:0]            rows = 16'd0, cols = 16'd0;
    reg                    w_valid = 1'b0;
    reg  [(LANES*4)-1:0]   w_data = {(LANES*4){1'b0}};
    wire                   w_ready, y_valid, done;
    wire [15:0]            y_index;
    wire signed [31:0]     y_data;
    wire [31:0]            beats, clocks;

    gemm_int4 #(.LANES(LANES), .MAX_COLS(512), .MAX_ROWS(64)) dut (
        .clk(clk), .rst_n(rst_n),
        .a_wr(a_wr), .a_addr(a_addr), .a_data(a_data),
        .start(start), .rows(rows), .cols(cols),
        .w_valid(w_valid), .w_data(w_data), .w_ready(w_ready),
        .y_valid(y_valid), .y_index(y_index), .y_data(y_data),
        .done(done), .beats(beats), .clocks(clocks)
    );

    always #5 clk = ~clk;

    integer errors = 0;
    integer i, r, seed, L;

    reg signed [7:0] act [0:COLS-1];
    reg signed [3:0] wgt [0:ROWS-1][0:COLS-1];

    /* Every row's result is captured as it appears, so a lost or duplicated row shows up as a
     * missing or doubled entry rather than being masked by the ones that were right. */
    reg signed [31:0] got [0:ROWS-1];
    reg [7:0]         hits [0:ROWS-1];
    reg               done_seen = 1'b0;

    always @(posedge clk) begin
        if (y_valid) begin
            got[y_index]  <= y_data;
            hits[y_index] <= hits[y_index] + 8'd1;
        end
        if (done) done_seen <= 1'b1;
    end

    function signed [31:0] expected(input integer rr);
        integer k;
        reg signed [31:0] s;
        begin
            s = 32'sd0;
            for (k = 0; k < COLS; k = k + 1) s = s + (wgt[rr][k] * act[k]);
            expected = s;
        end
    endfunction

    initial begin
        $display("");
        $display("===============================================================");
        $display("gemm_int4 -- %0dx%0d INT4 layer, %0d lanes", ROWS, COLS, LANES);
        $display("===============================================================");
        $display("");

        seed = 32'h0BADC0DE;
        for (i = 0; i < COLS; i = i + 1) act[i] = $random(seed);
        for (r = 0; r < ROWS; r = r + 1)
            for (i = 0; i < COLS; i = i + 1) wgt[r][i] = $random(seed);
        for (r = 0; r < ROWS; r = r + 1) hits[r] = 8'd0;

        repeat (4) @(negedge clk);
        rst_n = 1'b1;
        repeat (2) @(negedge clk);

        /* Activations once, for the whole matrix. */
        for (i = 0; i < COLS; i = i + LANES) begin
            @(negedge clk);
            a_wr   = 1'b1;
            a_addr = (i / LANES);
            for (L = 0; L < LANES; L = L + 1) a_data[(L*8) +: 8] = act[i + L];
        end
        @(negedge clk);
        a_wr = 1'b0;

        @(negedge clk);
        rows  = ROWS[15:0];
        cols  = COLS[15:0];
        start = 1'b1;
        @(negedge clk);
        start = 1'b0;

        /* Stream the matrix row-major, with a gap every fifth beat so back-pressure is exercised
         * rather than assumed. A real DDR3 feed will not deliver a beat every clock either. */
        begin : feed
            integer rr, cc, gap, guard;
            rr = 0; cc = 0; gap = 0; guard = 0;
            while (rr < ROWS && guard < 200000) begin
                @(negedge clk);
                if (gap == 4) begin
                    w_valid = 1'b0;
                    gap = 0;
                end
                else begin
                    w_valid = 1'b1;
                    for (L = 0; L < LANES; L = L + 1)
                        w_data[(L*4) +: 4] = ((cc + L) < COLS) ? wgt[rr][cc + L] : -4'sd8;
                    gap = gap + 1;
                end
                @(posedge clk);
                if (w_valid && w_ready) begin
                    cc = cc + LANES;
                    if (cc >= COLS) begin cc = 0; rr = rr + 1; end
                end
                guard = guard + 1;
            end
            @(negedge clk);
            w_valid = 1'b0;
        end

        i = 0;
        while (!done_seen && i < 2000) begin @(posedge clk); i = i + 1; end
        repeat (3) @(negedge clk);

        /* SECOND PASS: a beat offered on EVERY clock, no gaps at all.
         *
         * The pass above leaves a gap every fifth beat, which means it never offers a beat at a
         * moment the core is not ready -- so it cannot tell w_ready from a constant 1. Two mutations
         * proved that by surviving: forcing w_ready high, and counting beats without consulting it.
         * Both are invisible unless something pushes harder than the core can take.
         *
         * Holding valid high does exactly that. Any clock where the core is not ready and the
         * testbench believes it is now shows up as a wrong beat count. */
        begin : back_to_back
            integer rr, cc, L2, guard2, accepted2;
            done_seen = 1'b0;
            for (rr = 0; rr < ROWS; rr = rr + 1) hits[rr] = 8'd0;
            @(negedge clk);
            start = 1'b1;
            @(negedge clk);
            start = 1'b0;

            rr = 0; cc = 0; guard2 = 0; accepted2 = 0;
            while (rr < ROWS && guard2 < 200000) begin
                @(negedge clk);
                w_valid = 1'b1;                       /* never lowered */
                for (L2 = 0; L2 < LANES; L2 = L2 + 1)
                    w_data[(L2*4) +: 4] = ((cc + L2) < COLS) ? wgt[rr][cc + L2] : -4'sd8;
                @(posedge clk);
                if (w_ready) begin
                    accepted2 = accepted2 + 1;
                    cc = cc + LANES;
                    if (cc >= COLS) begin cc = 0; rr = rr + 1; end
                end
                guard2 = guard2 + 1;
            end
            @(negedge clk);
            w_valid = 1'b0;

            i = 0;
            while (!done_seen && i < 2000) begin @(posedge clk); i = i + 1; end
            repeat (3) @(negedge clk);

            if (accepted2 !== ROWS * ((COLS + LANES - 1) / LANES)) begin
                $display("  FAIL  back-to-back: ready was asserted %0d times, expected %0d",
                         accepted2, ROWS * ((COLS + LANES - 1) / LANES));
                errors = errors + 1;
            end
            if (beats !== accepted2[31:0]) begin
                $display("  FAIL  back-to-back: core counted %0d beats, testbench sent %0d",
                         beats, accepted2);
                errors = errors + 1;
            end
            for (rr = 0; rr < ROWS; rr = rr + 1) begin
                if (hits[rr] !== 8'd1) begin
                    $display("  FAIL  back-to-back: row %0d produced %0d results", rr, hits[rr]);
                    errors = errors + 1;
                end
                else if (got[rr] !== expected(rr)) begin
                    $display("  FAIL  back-to-back: row %0d wrong", rr);
                    errors = errors + 1;
                end
            end
            /* AFTER done, THE CORE MUST STOP ADVERTISING READY.
             *
             * A mutation removing the last-row guard on the re-arm survived every other check. The
             * reason it is invisible: the wrapper stops being busy, so no result is published and
             * nothing looks wrong. But the inner core has been re-armed on a phantom row, so
             * w_ready stays HIGH, and a feeder that keeps pushing has the next matrix's opening
             * beats swallowed into a stale accumulator. Silent, and it would corrupt one row per
             * matrix forever.
             *
             * Checking ready after done is the cheapest way to see it. */
            repeat (4) @(negedge clk);
            if (w_ready !== 1'b0) begin
                $display("  FAIL  ready is still asserted after done -- the core was left armed");
                errors = errors + 1;
            end

            if (errors == 0)
                $display("  back-to-back feed: %0d beats, no gaps, all rows correct", accepted2);
        end

        if (!done_seen) begin
            $display("  FAIL  the layer never finished");
            errors = errors + 1;
        end

        for (r = 0; r < ROWS; r = r + 1) begin
            if (hits[r] !== 8'd1) begin
                $display("  FAIL  row %0d produced %0d results, expected exactly 1", r, hits[r]);
                errors = errors + 1;
            end
            else if (got[r] !== expected(r)) begin
                $display("  FAIL  row %0d: %0d, expected %0d", r, got[r], expected(r));
                errors = errors + 1;
            end
        end

        /* The beat count is CHECKED, not merely printed.
         *
         * A mutation that counted beats without regard to w_ready survived the first pass, because
         * this figure was displayed and never asserted on. Anything reported but unverified is
         * decoration, and decoration is where wrong numbers hide. Expected is exactly one beat per
         * LANES columns, per row, with a ragged last beat still counting as one. */
        begin : check_beats
            integer want_beats;
            want_beats = ROWS * ((COLS + LANES - 1) / LANES);
            if (beats !== want_beats[31:0]) begin
                $display("  FAIL  consumed %0d beats, expected exactly %0d", beats, want_beats);
                errors = errors + 1;
            end
        end

        if (errors == 0) begin
            $display("  all %0d rows correct, each emitted exactly once", ROWS);
            $display("");
            $display("  weight beats consumed : %0d", beats);
            $display("  clocks from start     : %0d", clocks);
            $display("  duty cycle            : %0d%%  (fed with a gap every 5th beat)",
                     (beats * 100) / clocks);
            $display("");
            $display("  a duty cycle below 100%% means the core is waiting on the feed, which is");
            $display("  what memory bound looks like and where the bottleneck belongs.");
        end

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
