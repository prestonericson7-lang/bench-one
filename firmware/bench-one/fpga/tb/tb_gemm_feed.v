/* ===========================================================================================
 *  tb_gemm_feed.v -- how many lanes should the FPGA actually use?
 * ===========================================================================================
 *
 *  gemm_int4 is verified correct elsewhere. This asks a different question: fed at the rate a real
 *  DDR3 can deliver, how much of the core sits idle, and therefore how many lanes are worth
 *  building?
 *
 *  THE ARITHMETIC THIS IS CHECKING
 *  -------------------------------
 *  Each beat carries LANES weights at 4 bits, so LANES/2 bytes. At 100 MHz a core fed every clock
 *  consumes LANES * 50 MB/s:
 *
 *      LANES = 32   ->  1.6 GB/s
 *      LANES = 64   ->  3.2 GB/s
 *      LANES = 128  ->  6.4 GB/s
 *
 *  A Zynq's PS DDR3 supplies roughly 2.5 GB/s through the AXI HP ports. So the duty cycle should
 *  come out as supply divided by demand, and lanes past the point where those meet buy nothing:
 *
 *      LANES = 32   ->  demand 1.6, supply 2.5  ->  100%, memory has spare
 *      LANES = 64   ->  demand 3.2, supply 2.5  ->  78%
 *      LANES = 128  ->  demand 6.4, supply 2.5  ->  39%
 *
 *  Prediction 2 in docs/21 said "below 60% at 64 lanes". That was sloppy: 2.5/3.2 is 78%, not
 *  below 60. This testbench exists partly to catch that kind of error before hardware does, and it
 *  did.
 *
 *  HOW THE FEED IS THROTTLED
 *  --------------------------
 *  FEED_NUM beats offered per FEED_DEN clocks, spread evenly rather than in a burst. Bursts would
 *  flatter the core: it would run flat out during one and idle between, and the average would look
 *  the same while hiding whether the pipeline stalls. An even feed is also closer to what a DMA
 *  reading sequentially from DDR3 actually delivers.
 *
 *  RUN
 *      iverilog -g2005 -DLANES=64 -DFEED_NUM=78 -DFEED_DEN=100 -o f.vvp \
 *          ../rtl/gemv_int4.v ../rtl/gemm_int4.v tb_gemm_feed.v && vvp f.vvp
 * ===========================================================================================
 */

`default_nettype none
`timescale 1ns / 1ps

`ifndef LANES
  `define LANES 64
`endif
`ifndef FEED_NUM
  `define FEED_NUM 100
`endif
`ifndef FEED_DEN
  `define FEED_DEN 100
`endif

module tb_gemm_feed;

    localparam integer LANES = `LANES;
    localparam integer ROWS  = 64;
    localparam integer COLS  = 512;
    localparam integer FNUM  = `FEED_NUM;
    localparam integer FDEN  = `FEED_DEN;

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

    gemm_int4 #(.LANES(LANES), .MAX_COLS(COLS), .MAX_ROWS(ROWS)) dut (
        .clk(clk), .rst_n(rst_n),
        .a_wr(a_wr), .a_addr(a_addr), .a_data(a_data),
        .start(start), .rows(rows), .cols(cols),
        .w_valid(w_valid), .w_data(w_data), .w_ready(w_ready),
        .y_valid(y_valid), .y_index(y_index), .y_data(y_data),
        .done(done), .beats(beats), .clocks(clocks)
    );

    always #5 clk = ~clk;          /* 100 MHz */

    integer i, L, credit, offered, accepted, total_beats, guard;
    reg done_seen = 1'b0;
    always @(posedge clk) if (done) done_seen <= 1'b1;

    initial begin
        rows = ROWS[15:0];
        cols = COLS[15:0];
        total_beats = ROWS * ((COLS + LANES - 1) / LANES);

        repeat (4) @(negedge clk);
        rst_n = 1'b1;
        repeat (2) @(negedge clk);

        for (i = 0; i < COLS; i = i + LANES) begin
            @(negedge clk);
            a_wr = 1'b1; a_addr = (i / LANES);
            for (L = 0; L < LANES; L = L + 1) a_data[(L*8) +: 8] = 8'sd3;
        end
        @(negedge clk);
        a_wr = 1'b0;

        @(negedge clk);
        start = 1'b1;
        @(negedge clk);
        start = 1'b0;

        /* Throttle by credit: FNUM credits accrue every FDEN clocks, one spent per beat offered.
         * Spreading them this way rather than bursting is what makes the duty cycle mean
         * "how often the memory could feed it" instead of "how big the burst was". */
        credit = 0; offered = 0; accepted = 0; guard = 0;
        while (!done_seen && guard < 2000000) begin
            @(negedge clk);
            credit = credit + FNUM;
            if (credit >= FDEN && accepted < total_beats) begin
                credit = credit - FDEN;
                w_valid = 1'b1;
                for (L = 0; L < LANES; L = L + 1) w_data[(L*4) +: 4] = 4'sd2;
                offered = offered + 1;
            end
            else begin
                w_valid = 1'b0;
            end
            @(posedge clk);
            if (w_valid && w_ready) accepted = accepted + 1;
            guard = guard + 1;
        end
        w_valid = 1'b0;
        repeat (3) @(negedge clk);

        $display("");
        $display("  LANES %0d, fed %0d beats per %0d clocks", LANES, FNUM, FDEN);
        if (!done_seen) begin
            $display("  NEVER FINISHED after %0d clocks", guard);
        end
        else begin
            $display("    beats consumed : %0d of %0d expected", beats, total_beats);
            $display("    clocks         : %0d", clocks);
            $display("    duty cycle     : %0d%%", (beats * 100) / clocks);
            /* Bytes per beat is LANES/2, so weights per second at 100 MHz is
             * beats/clocks * LANES/2 * 100e6, expressed in GB/s. */
            $display("    weights        : %0d.%02d GB/s at 100 MHz",
                     (beats * LANES * 50) / (clocks * 1000),
                     (((beats * LANES * 50) / clocks) % 1000) / 10);
            if (beats !== total_beats[31:0])
                $display("    WRONG BEAT COUNT -- the throttle is losing beats");
        end
        $finish;
    end

endmodule

`default_nettype wire
