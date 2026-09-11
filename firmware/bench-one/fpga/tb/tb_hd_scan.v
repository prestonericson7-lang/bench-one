/* ===========================================================================================
 *  tb_hd_scan.v -- self-checking testbench for hd_scan.v
 * ===========================================================================================
 *
 *  This is the Verilog half of the verification. fpga/tb/hd_scan_model.py checks the DESIGN by
 *  modelling the state machine in Python and cross-checking it against bench_hdc_shard.c's rules;
 *  it cannot catch a Verilog mistake, because it is not Verilog. This runs the real RTL.
 *
 *  Every case below is one a mutation test proved matters. Removing the S_RUN exit on
 *  vec_done == r_total failed 30% of random Python trials. Turning strict less-than into
 *  less-or-equal failed every tie. Committing `acc` instead of `acc_next` dropped the last beat of
 *  every vector. So those are directed tests here, with deterministic data, rather than left to
 *  chance.
 *
 *  THE DATA IS ARRANGED SO EACH CASE HAS ONE UNAMBIGUOUS ANSWER
 *  ------------------------------------------------------------
 *  Two random 8192-bit vectors sit about 4096 bits apart, so anything deliberately placed nearer
 *  than that wins by a wide margin and the expected result cannot drift with the seed:
 *
 *      vec[3]  = the query with 208 bits flipped   -> distance 208, the nearest of the first eight
 *      vec[7]  = an exact copy of vec[3]           -> the same distance, so a guaranteed TIE
 *      vec[9]  = the query itself                  -> distance 0, kept out of the first eight
 *      others  = random                            -> about 4096
 *
 *  vec[9] is at index 9 on purpose. Putting the perfect match inside the first eight would win
 *  every comparison and the tie between 3 and 7 would never be reached, which is how the first
 *  version of this file passed while testing nothing.
 *
 *  RUN
 *      iverilog -g2005 -o tb.vvp ../rtl/hd_popcount.v ../rtl/hd_scan.v tb_hd_scan.v
 *      vvp tb.vvp
 * ===========================================================================================
 */

`default_nettype none
`timescale 1ns / 1ps

module tb_hd_scan;

    localparam integer HD_BITS = 8192;
    localparam integer W       = 512;
    localparam integer BEATS   = HD_BITS / W;      /* 16 */
    localparam integer NVEC    = 12;
    localparam integer NEAR    = 13;               /* bits flipped per beat: 13 x 16 = 208 */

    localparam [15:0] HD_NO_SLOT = 16'hFFFF;
    localparam [31:0] HD_FAR     = 32'hFFFFFFFF;

    reg              clk = 1'b0;
    reg              rst_n = 1'b0;
    reg              q_wr = 1'b0;
    reg  [3:0]       q_addr = 4'd0;
    reg  [W-1:0]     q_data = {W{1'b0}};
    reg  [15:0]      cfg_node = 16'd0, cfg_query_id = 16'd0, cfg_base = 16'd0, cfg_total = 16'd0;
    reg              start = 1'b0, halt = 1'b0;
    reg              s_valid = 1'b0;
    reg  [W-1:0]     s_data = {W{1'b0}};
    wire             s_ready, busy, done;
    wire [15:0]      o_best_local, o_scanned;
    wire [31:0]      o_best_dist;
    wire [127:0]     o_wire;

    hd_scan #(.HD_BITS(HD_BITS), .W(W)) dut (
        .clk(clk), .rst_n(rst_n),
        .q_wr(q_wr), .q_addr(q_addr), .q_data(q_data),
        .cfg_node(cfg_node), .cfg_query_id(cfg_query_id),
        .cfg_base(cfg_base), .cfg_total(cfg_total),
        .start(start), .halt(halt),
        .s_valid(s_valid), .s_data(s_data), .s_ready(s_ready),
        .busy(busy), .done(done),
        .o_best_local(o_best_local), .o_best_dist(o_best_dist), .o_scanned(o_scanned),
        .o_wire(o_wire)
    );

    always #5 clk = ~clk;                          /* 100 MHz */

    /* `done` is a single cycle. Latching it is not tidiness: a scan that ends by itself pulses
     * done while the testbench is still busy driving the stream, and the first version of this
     * file missed it and reported a core that never finished. */
    reg done_seen = 1'b0;
    always @(posedge clk) if (done) done_seen <= 1'b1;

    reg [W-1:0] query [0:BEATS-1];
    reg [W-1:0] vec   [0:NVEC-1][0:BEATS-1];
    reg [W-1:0] junk  [0:BEATS*4-1];

    integer errors = 0;
    integer i, j, k;

    task expect16(input [255:0] what, input [15:0] got, input [15:0] want);
        begin
            if (got !== want) begin
                $display("  FAIL  %0s: got %0d, expected %0d", what, got, want);
                errors = errors + 1;
            end
        end
    endtask

    task expect32(input [255:0] what, input [31:0] got, input [31:0] want);
        begin
            if (got !== want) begin
                $display("  FAIL  %0s: got %0d, expected %0d", what, got, want);
                errors = errors + 1;
            end
        end
    endtask

    /* Reference distance, counted one bit at a time so it shares no arithmetic with the DUT. */
    function integer ref_dist(input integer which);
        integer b, bit_i, d;
        begin
            d = 0;
            for (b = 0; b < BEATS; b = b + 1)
                for (bit_i = 0; bit_i < W; bit_i = bit_i + 1)
                    if (vec[which][b][bit_i] !== query[b][bit_i]) d = d + 1;
            ref_dist = d;
        end
    endfunction

    /* Expected winner over the first `n` vectors, by hd_scan_chunk's rule.
     *
     * The running minimum is a signed `integer` and the sentinel is a large POSITIVE value, not
     * HD_FAR. Verilog's integer is signed, so seeding it with 32'hFFFFFFFF makes it -1 and every
     * comparison below fails silently -- which is exactly what the first version of this file did,
     * reporting eight failures against a correct core. */
    reg [31:0] exp_dist;
    reg [15:0] exp_local;
    integer    best_d, best_i, d_i;

    task compute_expected(input integer n);
        begin
            best_d = 32'h7FFFFFFF;
            best_i = -1;
            for (d_i = 0; d_i < n; d_i = d_i + 1) begin
                if (ref_dist(d_i) < best_d) begin          /* STRICTLY less than */
                    best_d = ref_dist(d_i);
                    best_i = d_i;
                end
            end
            if (best_i < 0) begin
                exp_local = HD_NO_SLOT;
                exp_dist  = HD_FAR;
            end
            else begin
                exp_local = best_i[15:0];
                exp_dist  = best_d[31:0];
            end
        end
    endtask

    task load_query;
        begin
            for (i = 0; i < BEATS; i = i + 1) begin
                @(negedge clk);
                q_wr   = 1'b1;
                q_addr = i[3:0];
                q_data = query[i];
            end
            @(negedge clk);
            q_wr = 1'b0;
        end
    endtask

    task begin_scan(input [15:0] total);
        begin
            @(negedge clk);
            cfg_node     = 16'h002A;
            cfg_query_id = 16'hBEEF;
            cfg_base     = 16'd1000;
            cfg_total    = total;
            done_seen    = 1'b0;
            start        = 1'b1;
            @(negedge clk);
            start = 1'b0;
        end
    endtask

    /* Feed beats from vec[] in order, with a gap every third beat so the ready/valid handshake is
     * exercised rather than assumed. Stops early if the core finishes on its own. */
    task feed(input integer nbeats);
        integer sent, gap;
        begin
            sent = 0;
            gap  = 0;
            while (sent < nbeats && !done_seen) begin
                @(negedge clk);
                if (gap == 2) begin
                    s_valid = 1'b0;
                    gap = 0;
                end
                else begin
                    s_valid = 1'b1;
                    s_data  = vec[sent / BEATS][sent % BEATS];
                    gap = gap + 1;
                end
                @(posedge clk);
                if (s_valid && s_ready) sent = sent + 1;
            end
            @(negedge clk);
            s_valid = 1'b0;
        end
    endtask

    /* Offer beats the shard does not contain. A correct core has already stopped accepting, so
     * `taken` stays at zero and the loop ends on done_seen. */
    task feed_junk(input integer nbeats, output integer taken);
        integer sent, guard;
        begin
            sent  = 0;
            guard = 0;
            taken = 0;
            while (sent < nbeats && guard < 4000) begin
                @(negedge clk);
                s_valid = 1'b1;
                s_data  = junk[sent];
                @(posedge clk);
                if (s_ready) begin
                    sent  = sent + 1;
                    taken = taken + 1;
                end
                guard = guard + 1;
            end
            @(negedge clk);
            s_valid = 1'b0;
        end
    endtask

    task wait_done;
        integer guard;
        begin
            guard = 0;
            while (!done_seen && guard < 20000) begin
                @(posedge clk);
                guard = guard + 1;
            end
            if (!done_seen) begin
                $display("  FAIL  core never raised done");
                errors = errors + 1;
            end
            @(negedge clk);
        end
    endtask

    integer junk_taken;

    initial begin
        $display("");
        $display("===============================================================");
        $display("tb_hd_scan -- %0d-bit vectors, %0d bits per beat, %0d beats", HD_BITS, W, BEATS);
        $display("===============================================================");
        $display("");

        for (i = 0; i < BEATS; i = i + 1)
            query[i] = {$random, $random, $random, $random, $random, $random, $random, $random,
                        $random, $random, $random, $random, $random, $random, $random, $random};

        for (i = 0; i < NVEC; i = i + 1)
            for (j = 0; j < BEATS; j = j + 1)
                vec[i][j] = {$random, $random, $random, $random, $random, $random, $random, $random,
                             $random, $random, $random, $random, $random, $random, $random, $random};

        for (i = 0; i < BEATS*4; i = i + 1)
            junk[i] = {$random, $random, $random, $random, $random, $random, $random, $random,
                       $random, $random, $random, $random, $random, $random, $random, $random};

        /* vec[3] = the query with NEAR bits flipped in every beat. Decisively the nearest of the
         * first eight, and by a margin no random seed can close. */
        for (j = 0; j < BEATS; j = j + 1) begin
            vec[3][j] = query[j];
            for (k = 0; k < NEAR; k = k + 1)
                vec[3][j][(k * 37) % W] = ~vec[3][j][(k * 37) % W];
        end
        /* vec[7] is the same vector again: identical distance, so the tie is guaranteed. */
        for (j = 0; j < BEATS; j = j + 1) vec[7][j] = vec[3][j];
        /* vec[9] is the query, distance zero, deliberately outside the first eight. */
        for (j = 0; j < BEATS; j = j + 1) vec[9][j] = query[j];

        repeat (4) @(negedge clk);
        rst_n = 1'b1;
        repeat (2) @(negedge clk);

        load_query();

        /* ---- CASE 1: a whole shard, ending by itself ---------------------------------- */
        $display("[1] full scan of %0d vectors, core terminates on its own", NVEC);
        begin_scan(NVEC[15:0]);
        feed(NVEC * BEATS);
        wait_done();
        compute_expected(NVEC);
        expect16("best_local", o_best_local, exp_local);
        expect32("best_dist",  o_best_dist,  exp_dist);
        expect16("scanned",    o_scanned,    NVEC[15:0]);
        expect16("winner is the copy of the query at index 9", o_best_local, 16'd9);
        expect32("its distance is zero", o_best_dist, 32'd0);

        /* ---- CASE 2: a tie must go to the lower index ---------------------------------- */
        $display("[2] vectors 3 and 7 are identical and nearest: 3 must win");
        begin_scan(NVEC[15:0]);
        feed(8 * BEATS);
        halt = 1'b1;
        wait_done();
        halt = 1'b0;
        compute_expected(8);
        expect16("best_local over the first 8", o_best_local, exp_local);
        expect16("the tie went to the lower index", o_best_local, 16'd3);
        expect32("at the planted distance", o_best_dist, 32'd208);

        /* ---- CASE 3: halted mid-vector, which must contribute nothing ------------------ */
        $display("[3] halt three beats into vector 4: scanned must be 4, not 5");
        begin_scan(NVEC[15:0]);
        feed(4 * BEATS + 3);
        halt = 1'b1;
        wait_done();
        halt = 1'b0;
        compute_expected(4);
        expect16("scanned counts whole vectors only", o_scanned, 16'd4);
        expect16("best_local", o_best_local, exp_local);
        expect32("best_dist ignores the partial vector", o_best_dist, exp_dist);

        /* ---- CASE 4: nothing scanned at all -------------------------------------------- */
        $display("[4] halted before any vector completes");
        begin_scan(NVEC[15:0]);
        feed(BEATS - 1);
        halt = 1'b1;
        wait_done();
        halt = 1'b0;
        expect16("scanned", o_scanned, 16'd0);
        expect16("best_local is HD_NO_SLOT", o_best_local, HD_NO_SLOT);
        expect32("best_dist is HD_FAR", o_best_dist, HD_FAR);

        /* ---- CASE 5: the stream overruns the shard ------------------------------------- */
        $display("[5] stream keeps running past the end of a 6-vector shard");
        begin_scan(16'd6);
        feed(6 * BEATS);
        feed_junk(BEATS * 3, junk_taken);
        wait_done();
        compute_expected(6);
        expect16("scanned stops at total", o_scanned, 16'd6);
        expect16("best_local is inside the shard", o_best_local, exp_local);
        expect32("best_dist", o_best_dist, exp_dist);
        if (o_best_local >= 16'd6) begin
            $display("  FAIL  best_local %0d points outside a 6-vector shard", o_best_local);
            errors = errors + 1;
        end
        if (junk_taken != 0) begin
            $display("  FAIL  core accepted %0d beats past the end of the shard", junk_taken);
            errors = errors + 1;
        end

        /* ---- CASE 6: the packet ------------------------------------------------------- */
        $display("[6] the 16-byte reply is hd_partial_pack, little-endian");
        expect16("wire node",       o_wire[15:0],    16'h002A);
        expect16("wire query_id",   o_wire[31:16],   16'hBEEF);
        expect16("wire base",       o_wire[47:32],   16'd1000);
        expect16("wire best_local", o_wire[63:48],   o_best_local);
        expect32("wire best_dist",  o_wire[95:64],   o_best_dist);
        expect16("wire scanned",    o_wire[111:96],  o_scanned);
        expect16("wire total",      o_wire[127:112], 16'd6);

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
