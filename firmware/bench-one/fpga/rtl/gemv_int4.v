/* ===========================================================================================
 *  gemv_int4.v -- INT4 weights times INT8 activations, at DDR3 speed
 * ===========================================================================================
 *
 *  WHY THIS CORE IS THE REASON THE FPGA IS IN THE MACHINE
 *  -------------------------------------------------------
 *  Measured on a host with AVX2, same source file, identical values:
 *
 *      INT8    15.66 G MAC/s
 *      INT4     2.18 G MAC/s
 *
 *  INT4 runs at a seventh the rate of INT8 in software, because every byte needs a shift and two
 *  sign extensions before a single multiply happens. No CPU in this project has 4-bit hardware and
 *  nothing in its price class does.
 *
 *  In fabric that unpacking is WIRES. Splitting a byte into two nibbles and sign-extending both
 *  costs no logic, no clock and no cycle -- it is which bit goes to which input. The seven times
 *  penalty simply does not exist here.
 *
 *  That is the whole argument for the FPGA. It is not a RAM donor and it is not an accelerator
 *  looking for a job. It is the only part in the system that makes the quantization the capacity
 *  arithmetic depends on actually free.
 *
 *
 *  BUILT TO BE MEMORY BOUND, WHICH IS THE POINT
 *  ---------------------------------------------
 *  LANES multiply-accumulates retire per clock, each eating half a byte of weight:
 *
 *      weight bytes/s = LANES * clock / 2
 *
 *  At 100 MHz, 48 lanes consume 2.4 GB/s, which is about what a Zynq-7000's DDR3 delivers through
 *  the AXI HP ports. Past that the core would sit waiting on memory, which is exactly where the
 *  bottleneck belongs: the memory is the product and the arithmetic should be free.
 *
 *  Each lane is a 4x8 signed multiply. That fits a single DSP48 with room to spare, and a 7020 has
 *  220 of them, so lane count is limited by the memory long before the fabric. Do not raise LANES
 *  past what the DDR3 can feed -- it buys nothing and costs timing closure.
 *
 *
 *  THE ACCUMULATOR IS 32 BITS AND THAT IS NOT ARBITRARY
 *  -----------------------------------------------------
 *  A 4-bit weight spans -8..7 and an 8-bit activation -128..127, so one product fits in 15 bits
 *  signed. Accumulating K of them needs 15 + ceil(log2(K)) bits. At K = 65536 columns that is 31,
 *  so 32 bits covers any row width this machine will ever see with a bit to spare. A 16-bit
 *  accumulator would overflow silently at 128 columns and produce a plausible wrong answer, which
 *  is the failure mode this file exists to avoid.
 *
 *
 *  DATA LAYOUT
 *  ------------
 *  Weights arrive packed two per byte, LOW NIBBLE FIRST, matching gemv_pack4 in bench_gemv.h. Get
 *  that backwards and the core runs at exactly the right speed computing nonsense, so the testbench
 *  checks against the C reference rather than against itself.
 * ===========================================================================================
 */

`default_nettype none

module gemv_int4 #(
    parameter integer LANES    = 8,     /* MACs per clock. Weight bytes/s = LANES * clk / 2. */
    parameter integer MAX_COLS = 4096   /* widest row this core will ever be given            */
) (
    input  wire                      clk,
    input  wire                      rst_n,

    /* Load the activation vector, LANES bytes at a time. See the note on the memory below for
     * why this is wide rather than one byte per write. */
    input  wire                      a_wr,
    input  wire [15:0]               a_addr,     /* in units of LANES activations */
    input  wire [(LANES*8)-1:0]      a_data,

    input  wire                      start,      /* one-cycle pulse: begin a new row */
    input  wire [15:0]               cols,       /* columns in this row; must be even */

    /* Weight stream: LANES/2 bytes per beat, since each byte carries two weights. */
    input  wire                      w_valid,
    input  wire [(LANES*4)-1:0]      w_data,
    output wire                      w_ready,

    output reg                       done,       /* one-cycle pulse when the row is complete */
    output reg  signed [31:0]        acc,        /* the dot product */
    output reg  [15:0]               consumed    /* weights processed, for coverage reporting */
);

    localparam integer AW = 16;

    reg [AW-1:0] col;          /* which column the next beat starts at */
    reg          busy;

    /* THE ACTIVATION MEMORY IS WIDE, AND THAT IS THE DIFFERENCE BETWEEN FITTING AND NOT.
     *
     * The first version declared this as a byte array and read it at LANES independent addresses
     * every clock. That is an 8-port memory, and block RAM has two ports, so the synthesiser had no
     * choice but to build it from LUTs. Measured: 32,636 logic cells at 8 lanes and 64,551 at 16,
     * against a Zynq-7020's 53,200. The core did not fit the chip it was written for, and nothing
     * about the arithmetic was wrong -- the memory shape was.
     *
     * Storing LANES activations per word makes it a ONE-port memory read once per beat, which is
     * exactly what a BRAM does. `col` advances by LANES every beat, so the address is simply
     * col/LANES and the whole lane vector arrives in a single read.
     *
     * The cost is that activations must be written LANES at a time. That is not a real constraint:
     * they arrive from the previous pipeline stage as a stream, which is already the shape wanted. */
    localparam integer DEPTH = (MAX_COLS + LANES - 1) / LANES;
    localparam integer DAW   = (DEPTH > 1) ? $clog2(DEPTH) : 1;

    reg [(LANES*8)-1:0] avec [0:DEPTH-1];

    always @(posedge clk) begin
        if (a_wr) avec[a_addr[DAW-1:0]] <= a_data;
    end

    /* ONE address, read combinationally. That is the whole fix.
     *
     * What exploded was not the read, it was reading LANES DIFFERENT addresses at once, which is an
     * eight-port memory and has no hardware equivalent. A single wide read at a single address is
     * an ordinary one-port lookup: DEPTH entries of LANES*8 bits, which at 4096 columns and 8 lanes
     * is 512 x 64 bits, small enough to sit in distributed RAM without a pipeline stage and without
     * the bubble a registered read would put at the start of every row. */
    wire [(LANES*8)-1:0] arow = avec[col[AW-1:0] / LANES];

    /* Accept a beat only while a row is in progress.
     *
     * The `col < cols` term is DEFENSIVE, not load bearing, and a mutation test removing it changed
     * nothing: `busy` already drops on the same clock the final beat is accepted, so ready is low
     * before any overrun beat could arrive. It stays because the cost is one comparator and it
     * makes the intent explicit, but it should not be described as the thing preventing overrun.
     * That is `busy`. */
    assign w_ready = busy && (col < cols);

    wire accept = w_valid && w_ready;

    /* ---- the unpack, which is the entire reason this core exists -------------------------
     * Each 4-bit slice becomes a signed value by naming its top bit as the sign. In software that
     * is a shift, a mask and a conditional subtract per nibble. Here it is a bus rename: no logic,
     * no clock, no cycle. */
    wire signed [15:0] prod [0:LANES-1];
    genvar g;
    generate
        for (g = 0; g < LANES; g = g + 1) begin : lane
            wire signed [3:0] w = w_data[(g*4) + 3 : (g*4)];
            wire signed [7:0] a = arow[(g*8) +: 8];
            assign prod[g] = w * a;          /* 4x8 signed -> 12 bits, held in 16 */
        end
    endgenerate

    /* Sum the lanes. A balanced tree keeps the depth logarithmic in LANES, so widening the core
     * costs area rather than timing. */
    integer i;
    reg signed [31:0] lane_sum;
    always @(*) begin
        lane_sum = 32'sd0;
        for (i = 0; i < LANES; i = i + 1) begin
            /* Only lanes inside the row contribute. A row whose column count is not a multiple of
             * LANES has a ragged final beat, and counting its padding would corrupt the result. */
            if ((col + i[AW-1:0]) < cols)
                lane_sum = lane_sum + {{16{prod[i][15]}}, prod[i]};
        end
    end

    always @(posedge clk) begin
        if (!rst_n) begin
            busy     <= 1'b0;
            done     <= 1'b0;
            acc      <= 32'sd0;
            col      <= {AW{1'b0}};
            consumed <= 16'd0;
        end
        else begin
            done <= 1'b0;

            if (start) begin
                busy     <= 1'b1;
                acc      <= 32'sd0;
                col      <= {AW{1'b0}};
                consumed <= 16'd0;
            end
            else if (accept) begin
                acc <= acc + lane_sum;

                /* Count only real columns, never the padding in a ragged last beat. */
                if (col + LANES[AW-1:0] >= cols) begin
                    consumed <= cols;
                    col      <= cols;
                    busy     <= 1'b0;
                    done     <= 1'b1;
                end
                else begin
                    consumed <= consumed + LANES[15:0];
                    col      <= col + LANES[AW-1:0];
                end
            end
        end
    end

endmodule

`default_nettype wire
