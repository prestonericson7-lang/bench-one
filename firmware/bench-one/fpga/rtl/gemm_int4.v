/* ===========================================================================================
 *  gemm_int4.v -- a whole matrix, which is a transformer's linear layer
 * ===========================================================================================
 *
 *  gemv_int4 does one row. A layer is a matrix, so this walks rows: the activation vector is loaded
 *  once and reused across every one of them, the weights stream past continuously, and one output
 *  lands per row.
 *
 *  THE ACTIVATIONS ARE LOADED ONCE AND THAT IS THE WHOLE ECONOMY OF IT
 *  --------------------------------------------------------------------
 *  A 4096x4096 INT4 layer is 8 MB of weights and 8 KB of activations. The weights are read once and
 *  never revisited; the activations are read 4096 times. Putting the small thing in fabric and
 *  streaming the large thing past it is the only arrangement that makes sense, and it is why this
 *  design is memory bound by construction rather than by accident.
 *
 *  It is also why pipeline parallelism is the right decomposition for the machine as a whole. The
 *  same asymmetry holds one level up: weights are huge and stay put, activations are tiny and move.
 *
 *
 *  NO IDLE CYCLE BETWEEN ROWS
 *  --------------------------
 *  There used to be one, and a comment here argued it was too small to bother with. See the note on
 *  row_start below for the measurement that proved otherwise. Rows now run back to back.
 *
 *
 *  WHAT IT REPORTS AND WHY
 *  ------------------------
 *  `beats` counts every weight beat consumed across the whole matrix. Divided by the elapsed clocks
 *  it gives the real duty cycle, which is the number that says whether the core is starved by the
 *  memory feeding it or genuinely running flat out. A design that is memory bound should show a duty
 *  cycle below one, and if it ever shows one, the memory is faster than the arithmetic and LANES
 *  should go up.
 * ===========================================================================================
 */

`default_nettype none

module gemm_int4 #(
    parameter integer LANES    = 8,
    parameter integer MAX_COLS = 4096,
    parameter integer MAX_ROWS = 4096
) (
    input  wire                      clk,
    input  wire                      rst_n,

    /* Activation vector, LANES bytes per write, loaded once for the whole matrix. */
    input  wire                      a_wr,
    input  wire [15:0]               a_addr,
    input  wire [(LANES*8)-1:0]      a_data,

    input  wire                      start,      /* one-cycle pulse: begin the matrix */
    input  wire [15:0]               rows,
    input  wire [15:0]               cols,

    /* Weight stream, row-major, each row packed low nibble first. */
    input  wire                      w_valid,
    input  wire [(LANES*4)-1:0]      w_data,
    output wire                      w_ready,

    /* One result per row, as it completes. */
    output reg                       y_valid,
    output reg  [15:0]               y_index,
    output reg  signed [31:0]        y_data,

    output reg                       done,       /* one-cycle pulse when the matrix is finished */
    output reg  [31:0]               beats,      /* weight beats consumed, for duty cycle */
    output reg  [31:0]               clocks      /* clocks from start to done                */
);

    reg         busy;
    reg [15:0]  row;
    reg         row_start_q;

    wire        core_done;
    wire signed [31:0] core_acc;
    wire [15:0] core_consumed;
    wire        core_ready;

    /* Re-arm the core COMBINATIONALLY when it finishes a row.
     *
     * Registering this cost an extra idle clock between rows on top of the one the core already
     * takes to clear its accumulator. At 8 lanes and 4096 columns that was 0.2% and a comment here
     * said it was not worth fixing. That comment was only true for those numbers. A sweep against a
     * throttled feed showed the real shape:
     *
     *     LANES=16, 512 cols   32 beats a row   duty 94%
     *     LANES=32             16 beats a row   duty 88%
     *     LANES=64              8 beats a row   duty 64%
     *     LANES=128             4 beats a row   duty 33%
     *
     * The gap is a fixed cost per ROW, so its share grows as the row gets shorter in beats, which
     * is exactly what widening the core does. At 128 lanes a row is four beats and the idle time is
     * a fifth of the machine. Wide cores are the whole point, so the gap had to go. */
    wire        row_start = row_start_q | (busy & core_done & (row + 16'd1 < rows));

    gemv_int4 #(.LANES(LANES), .MAX_COLS(MAX_COLS)) core (
        .clk(clk), .rst_n(rst_n),
        .a_wr(a_wr), .a_addr(a_addr), .a_data(a_data),
        .start(row_start), .cols(cols),
        .w_valid(w_valid), .w_data(w_data), .w_ready(core_ready),
        .done(core_done), .acc(core_acc), .consumed(core_consumed)
    );

    /* Pass the inner core's back-pressure straight through. The wrapper never buffers a beat, so
     * there is nowhere for one to be lost or duplicated between the two. */
    assign w_ready = core_ready;

    always @(posedge clk) begin
        if (!rst_n) begin
            busy      <= 1'b0;
            row       <= 16'd0;
            row_start_q <= 1'b0;
            y_valid   <= 1'b0;
            y_index   <= 16'd0;
            y_data    <= 32'sd0;
            done      <= 1'b0;
            beats     <= 32'd0;
            clocks    <= 32'd0;
        end
        else begin
            y_valid   <= 1'b0;
            done      <= 1'b0;
            row_start_q <= 1'b0;

            if (w_valid && w_ready) beats <= beats + 32'd1;
            if (busy) clocks <= clocks + 32'd1;

            if (start) begin
                busy      <= 1'b1;
                row       <= 16'd0;
                beats     <= 32'd0;
                clocks    <= 32'd0;
                row_start_q <= 1'b1;
            end
            else if (busy && core_done) begin
                /* Publish this row's result, then either start the next row or finish. */
                y_valid <= 1'b1;
                y_index <= row;
                y_data  <= core_acc;

                if (row + 16'd1 >= rows) begin
                    busy <= 1'b0;
                    done <= 1'b1;
                end
                else begin
                    row       <= row + 16'd1;
                end
            end
        end
    end

endmodule

`default_nettype wire
