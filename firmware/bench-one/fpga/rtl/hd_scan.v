/* ===========================================================================================
 *  hd_scan.v -- the FPGA answering the same question a Teensy answers, in the same words
 * ===========================================================================================
 *
 *  WHAT THIS IS FOR
 *  -----------------
 *  A Teensy 4.1 holds a few hundred hypervectors and compares one in about six microseconds.
 *  A Zynq board here carries 1 GB of DDR3, which at 1 KB per 8192-bit vector is a little over a
 *  million of them on ONE board. The FPGA is therefore not a faster node. It is a different tier:
 *  the place the deep memory lives, streamed past a comparator at whatever rate DDR3 can feed.
 *
 *  Compute is not the constraint. A 512-bit XOR and popcount finishes in one clock, so at 100 MHz
 *  this core consumes 6.4 GB/s, which is more than the DDR3 on these boards delivers. That is the
 *  correct place for the bottleneck to sit: the memory is the product, and the arithmetic is free.
 *
 *
 *  THE CONTRACT, AND WHY IT IS THE WHOLE POINT
 *  --------------------------------------------
 *  The coordinator must not be able to tell an FPGA reply from a Teensy reply. Everything in this
 *  system rests on the merge being a monoid, so that answers can arrive late, out of order or
 *  twice and still combine to one result. That property is not a property of the C. It is a
 *  property of the ALGEBRA, and it only holds if every node obeys the same rules. One node that
 *  breaks ties differently is enough to make the same query return different answers depending on
 *  which reply the network happened to deliver first, and nothing would look broken.
 *
 *  So this core reproduces bench_hdc_shard.c exactly, and here is the list, taken from that file
 *  rather than remembered:
 *
 *    * WINNER IS STRICTLY-LESS-THAN, LOWEST INDEX WINS A TIE.
 *      hd_scan_chunk uses `d < best_dist`, plus a lowest-index tie-break. Because a scan walks its
 *      indices upward, plain strict less-than already gives lowest-index-wins, which is what this
 *      core implements. Using <= instead would hand ties to the HIGHEST index and silently break
 *      commutativity at the boundary between fabric and firmware.
 *
 *    * AN UNFINISHED VECTOR IS NOT A RESULT AND IS NOT COVERAGE.
 *      When halt arrives mid-vector, the partial distance is thrown away and `scanned` does not
 *      count it. Both halves of that matter. A partial sum over three of sixteen beats is a small
 *      number, and a small number is what a good match looks like, so committing it would produce
 *      a confident wrong answer. Counting it would inflate coverage, and coverage is what decides
 *      whether "not found" means "this is new" or "the nodes that knew never answered".
 *
 *    * EMPTY IS best_local = 0xFFFF AND best_dist = 0xFFFFFFFF.
 *      HD_NO_SLOT and HD_FAR from bench_hdc_shard.c. The merge tests best_local against 0xFFFF to
 *      decide whether a reply carries a candidate at all.
 *
 *    * THE PACKET IS 16 BYTES, LITTLE-ENDIAN.
 *      hd_partial_pack writes node, query_id, base, best_local, best_dist, scanned, total, and
 *      its put16/put32 are little-endian. o_wire below is that packet as one integer, so byte i
 *      is o_wire[8*i+7 : 8*i].
 *
 *
 *  STREAM ORDER
 *  -------------
 *  Vectors arrive back to back, BEATS beats each, in ascending local index starting at zero.
 *  `cfg_base` is what turns a local index into a global one at the coordinator, exactly as it does
 *  for a Teensy shard, so this core never needs to know where it sits in the whole memory.
 *
 *
 *  WIDTH AND TIMING -- USE W = 256
 *  -------------------------------
 *  One register stage sits between the XOR and the accumulator, so a full W-bit popcount tree gets
 *  a whole clock period. Measured with yosys synth_xilinx for xc7:
 *
 *        W = 128    455 LCs,  348 FFs,  no distributed RAM,  1.6 GB/s at 100 MHz
 *        W = 256    797 LCs,  475 FFs,  43 x RAM32M,         3.2 GB/s at 100 MHz
 *        W = 512  1,464 LCs,  730 FFs,  86 x RAM32M,         6.4 GB/s at 100 MHz
 *
 *  A Zynq-7000's 32-bit PS DDR3 gives roughly 3 GB/s through the AXI HP ports, so 256 bits per
 *  beat already saturates the memory. W = 512 doubles the logic and the timing difficulty to buy
 *  bandwidth the DDR3 cannot supply. The default below stays at 512 only because a board with
 *  wider or faster memory can use it; set it to 256 for the Zynq boards in this build.
 *
 *  Yosys estimates area, not the critical path. Only Vivado's timing analysis settles whether the
 *  nine-level tree at W = 512 closes at 100 MHz, and that is another reason to start at 256.
 * ===========================================================================================
 */

`default_nettype none

module hd_scan #(
    parameter integer HD_BITS = 8192,
    parameter integer W       = 512
) (
    input  wire         clk,
    input  wire         rst_n,

    /* --- query load. Write all BEATS words before pulsing start. ------------------------- */
    input  wire                          q_wr,
    input  wire [$clog2(HD_BITS/W)-1:0]  q_addr,
    input  wire [W-1:0]                  q_data,

    /* --- identity of this reply. Latched at start so a mid-scan change cannot corrupt it. - */
    input  wire [15:0]  cfg_node,
    input  wire [15:0]  cfg_query_id,
    input  wire [15:0]  cfg_base,
    input  wire [15:0]  cfg_total,

    input  wire         start,          /* one-cycle pulse                                   */
    input  wire         halt,           /* level: stop cleanly and report what was finished  */

    /* --- the memory stream ------------------------------------------------------------- */
    input  wire         s_valid,
    input  wire [W-1:0] s_data,
    output wire         s_ready,

    output reg          busy,
    output reg          done,           /* one-cycle pulse when the reply is valid           */

    output reg  [15:0]  o_best_local,
    output reg  [31:0]  o_best_dist,
    output reg  [15:0]  o_scanned,
    output wire [127:0] o_wire
);

    localparam integer BEATS = HD_BITS / W;
    localparam integer BAW   = (BEATS > 1) ? $clog2(BEATS) : 1;
    localparam integer PCW   = $clog2(W + 1);         /* popcount of one beat                */
    localparam integer ACCW  = $clog2(HD_BITS + 1);   /* a whole vector's distance            */

    localparam [15:0] HD_NO_SLOT = 16'hFFFF;
    localparam [31:0] HD_FAR     = 32'hFFFFFFFF;

    localparam [1:0] S_IDLE  = 2'd0,
                     S_RUN   = 2'd1,
                     S_DRAIN = 2'd2,
                     S_DONE  = 2'd3;

    reg [1:0] state;

    /* --- the query, one word per beat ---------------------------------------------------- */
    reg [W-1:0] qmem [0:BEATS-1];
    always @(posedge clk) begin
        if (q_wr) qmem[q_addr] <= q_data;
    end

    /* --- latched identity ---------------------------------------------------------------- */
    reg [15:0] r_node, r_qid, r_base, r_total;

    /* --- position ------------------------------------------------------------------------ */
    reg [BAW-1:0]  beat;             /* which beat of the current vector comes next          */
    reg [15:0]     vec_done;         /* vectors fully compared: this is both the running local
                                      * index and the `scanned` count, because a scan starts at
                                      * local index zero and never skips.                     */

    /* --- the one pipeline stage ---------------------------------------------------------- */
    reg [W-1:0]    x_r;
    reg            x_vld;
    reg            x_last;

    wire [PCW-1:0] pc;
    hd_popcount #(.W(W)) u_pc (.d(x_r), .cnt(pc));

    reg  [ACCW-1:0] acc;
    wire [ACCW-1:0] acc_next = acc + {{(ACCW-PCW){1'b0}}, pc};

    /* --- running best ------------------------------------------------------------------- */
    reg [31:0] best_dist;
    reg [15:0] best_local;

    /* Accept a beat only while running, not asked to stop, and not already finished. Dropping
     * ready is what makes the drain below possible: the pipeline empties on its own in one cycle.
     *
     * The vec_done != r_total term is defensive, not load bearing. A mutation test removing it
     * changed no result: the S_RUN exit below already stops the scan the cycle after the last
     * vector commits, and the single stray beat that can slip through in the meantime lands in a
     * partial vector the drain throws away. What it buys is one fewer beat pulled off a stream
     * that may be a DMA burst nobody wanted to shorten. The guard that genuinely matters is that
     * exit -- removing THAT fails 30% of trials, because the scan then runs on into whatever
     * follows the shard in memory and commits it as a candidate at an index the shard does not
     * contain. See fpga/tb/hd_scan_model.py. */
    assign s_ready = (state == S_RUN) && !halt && (vec_done != r_total);

    wire accept = s_valid && s_ready;

    always @(posedge clk) begin
        if (!rst_n) begin
            state      <= S_IDLE;
            busy       <= 1'b0;
            done       <= 1'b0;
            beat       <= {BAW{1'b0}};
            vec_done   <= 16'd0;
            acc        <= {ACCW{1'b0}};
            x_vld      <= 1'b0;
            x_last     <= 1'b0;
            best_dist  <= HD_FAR;
            best_local <= HD_NO_SLOT;
        end
        else begin
            done <= 1'b0;

            /* ---- stage 0: XOR the incoming beat against the query ---------------------- */
            if (accept) begin
                x_r    <= s_data ^ qmem[beat];
                x_vld  <= 1'b1;
                x_last <= (beat == (BEATS - 1));
                beat   <= (beat == (BEATS - 1)) ? {BAW{1'b0}} : (beat + 1'b1);
            end
            else begin
                x_vld  <= 1'b0;
            end

            /* ---- stage 1: popcount, accumulate, and commit on the last beat ------------ */
            if (x_vld) begin
                if (x_last) begin
                    /* STRICTLY less-than. See the contract note at the top: <= would give ties
                     * to the highest index and break the merge's commutativity. */
                    if ({{(32-ACCW){1'b0}}, acc_next} < best_dist) begin
                        best_dist  <= {{(32-ACCW){1'b0}}, acc_next};
                        best_local <= vec_done;
                    end
                    vec_done <= vec_done + 16'd1;
                    acc      <= {ACCW{1'b0}};
                end
                else begin
                    acc <= acc_next;
                end
            end

            /* ---- control -------------------------------------------------------------- */
            case (state)
            S_IDLE: begin
                if (start) begin
                    r_node     <= cfg_node;
                    r_qid      <= cfg_query_id;
                    r_base     <= cfg_base;
                    r_total    <= cfg_total;
                    beat       <= {BAW{1'b0}};
                    vec_done   <= 16'd0;
                    acc        <= {ACCW{1'b0}};
                    x_vld      <= 1'b0;
                    best_dist  <= HD_FAR;
                    best_local <= HD_NO_SLOT;
                    busy       <= 1'b1;
                    state      <= S_RUN;
                end
            end

            S_RUN: begin
                /* Two ways to leave: told to stop, or the shard is exhausted. A scan that only
                 * ended on halt would need the host to watch for completion and race the core to
                 * announce it; letting the core finish itself makes a full scan and a cut-short
                 * scan the same code path, with the same reporting. */
                if (halt || (vec_done == r_total)) state <= S_DRAIN;
            end

            S_DRAIN: begin
                if (!x_vld) begin
                    /* THE PARTIAL VECTOR IS THROWN AWAY HERE, and it is thrown away by doing
                     * nothing: beat is non-zero, so the last vector never reached its commit,
                     * so vec_done never counted it and best never saw its partial sum. Writing
                     * anything at all at this point is the bug. */
                    acc   <= {ACCW{1'b0}};
                    beat  <= {BAW{1'b0}};
                    state <= S_DONE;
                end
            end

            S_DONE: begin
                o_best_local <= best_local;
                o_best_dist  <= best_dist;
                o_scanned    <= vec_done;
                busy         <= 1'b0;
                done         <= 1'b1;
                state        <= S_IDLE;
            end
            endcase
        end
    end

    /* hd_partial_pack, byte for byte. Field order is node, query_id, base, best_local,
     * best_dist, scanned, total, and every field is little-endian, so laying them out with the
     * first field in the low bits makes byte i of the packet equal o_wire[8*i+7 : 8*i]. */
    assign o_wire = { r_total,        /* bytes 14-15 */
                      o_scanned,      /* bytes 12-13 */
                      o_best_dist,    /* bytes  8-11 */
                      o_best_local,   /* bytes  6-7  */
                      r_base,         /* bytes  4-5  */
                      r_qid,          /* bytes  2-3  */
                      r_node };       /* bytes  0-1  */

endmodule

`default_nettype wire
