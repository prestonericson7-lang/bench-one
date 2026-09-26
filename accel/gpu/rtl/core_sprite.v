// core_sprite.v -- SPRITE records: reads source rows over AXI3 HP2, writes the strip colour buffer.
//
// Record: w0 = SPRITE<<28 | flags (bit 24 COLORKEY), w1 = y<<11 | x, w2 = h<<11 | w,
//         w3 = src, w4 = stride, w5[15:0] = key.  (SPEC section 5 / gpu_proto.h)
// For strip s (rows 16s..16s+15): rows y+row in the strip with 0 <= row < h (and < 720),
// columns 0..min(w,1280-x)-1. Source word for (row, 4 columns) = src + row*stride + col*2,
// 8-byte aligned. Out-of-contract low address bits are ignored: x and w are used as multiples
// of 4 (bits [1:0] dropped), src and stride as multiples of 8 (bits [2:0] dropped).
//
// Per strip: addr0 = src + row0*stride (iterative shift-add, <= 10 cycles), then for each row
// issue INCR bursts: len = min(16, words left in row, words to the next 4 KB boundary).
// R data is always accepted (rready = 1) and written straight into the colour buffer with
// per-pixel enables (pixel != key or no COLORKEY). AXI returns ID-0 reads in order, so the
// R side just walks the same (row, word) sequence.
// The strip buffer word is 128 bits (8 pixels, core_tri); one 64-bit source beat is 4 pixels
// at a multiple of 4, i.e. one half of a strip word: the R side walks 4-pixel group addresses
// (row*320 + x/4) and writes strip word (group >> 1), half (group & 1) with 8 lane enables.
module core_sprite (
    input  wire              clk,
    input  wire              rst,
    input  wire [5:0]        strip,
    input  wire              start,          // pulse; only when idle
    /* verilator lint_off UNUSED */
    input  wire [23*32-1:0]  rec,
    /* verilator lint_on UNUSED */
    input  wire              go_ok,          // earlier primitives finished writing
    output wire              idle,
    // AXI3 read master (HP2)
    output reg  [31:0]       araddr,
    output reg  [3:0]        arlen,
    output reg               arvalid,
    input  wire              arready,
    input  wire [63:0]       rdata,
    input  wire              rvalid,
    output wire              rready,
    // colour buffer write (128-bit strip words, 8 pixel lanes)
    output reg  [7:0]        c_we,
    output reg  [11:0]       w_addr,
    output reg  [127:0]      c_wdata,
    output wire              w_active
);
    /* verilator lint_off UNUSED */
    wire [31:0] w0 = rec[0*32 +: 32];
    wire [31:0] w1 = rec[1*32 +: 32];
    wire [31:0] w2 = rec[2*32 +: 32];
    wire [31:0] w3 = rec[3*32 +: 32];
    wire [31:0] w4 = rec[4*32 +: 32];
    wire [31:0] w5 = rec[5*32 +: 32];
    /* verilator lint_on UNUSED */

    localparam ST_IDLE = 3'd0, ST_PREP = 3'd1, ST_MUL = 3'd2, ST_WAIT = 3'd3, ST_RUN = 3'd4;
    reg [2:0]  st;

    // captured fields
    reg [10:0] x, y, w, h;
    reg [31:0] src, stride;
    reg [15:0] key;
    reg        ck;

    // derived
    reg [9:0]  mb;          // row0 multiplier bits
    reg [31:0] acc, mm;
    reg [8:0]  nwords;      // words per row (1..320)
    reg [4:0]  nrows;       // rows in this strip (1..16)
    reg [12:0] rbase0;      // 4-pixel group address (row*320 + x/4) of the first row

    // AR side
    reg [31:0] a_addr, a_row;
    reg [8:0]  a_left;      // words left in the current row
    reg [4:0]  a_rows;      // rows left including current
    reg        a_calc;      // 1 = compute next burst, 0 = burst in arvalid / done
    reg        a_done;
    reg [4:0]  a_bl;

    // R side
    reg [8:0]  r_left;      // words left in row after the current
    reg [4:0]  r_rows;      // rows left including current
    reg [12:0] r_addr, r_rowaddr;   // 4-pixel group addresses (strip word = addr >> 1)
    reg        r_done;
    reg        q_v;
    reg [63:0] q_d;
    reg [12:0] q_a;
    reg        wv;

    assign rready   = 1'b1;
    assign idle     = (st == ST_IDLE);
    assign w_active = wv;

    wire [10:0] y0s = {1'b0, strip, 4'b0000};
    wire [10:0] y1s = {1'b0, strip, 4'b1111};

    // derived values from captured fields (used in ST_PREP)
    wire [10:0] ys     = (y > y0s) ? y : y0s;
    wire [11:0] yend   = {1'b0, y} + {1'b0, h} - 12'd1;
    /* verilator lint_off UNUSED */
    wire [11:0] ye     = (yend < {1'b0, y1s}) ? yend : {1'b0, y1s};
    wire [10:0] row0   = ys - y;
    wire [10:0] xspace = 11'd1280 - x;                   // x < 1280 (side table)
    wire [10:0] ncols  = (w < xspace) ? w : xspace;      // multiple of 4, >= 4
    /* verilator lint_on UNUSED */

    // next burst length: min(16, a_left, words to 4 KB boundary)
    wire [9:0]  to4k   = 10'd512 - {1'b0, a_addr[11:3]};
    // pixel enables of the captured beat (pixel != key or no COLORKEY)
    wire [3:0]  pen    = { !(ck && (q_d[63:48] == key)), !(ck && (q_d[47:32] == key)),
                           !(ck && (q_d[31:16] == key)), !(ck && (q_d[15:0]  == key)) };
    wire [9:0]  bl_a   = ({1'b0, a_left} < 10'd16) ? {1'b0, a_left} : 10'd16;
    /* verilator lint_off UNUSED */
    wire [9:0]  bl     = (to4k < bl_a) ? to4k : bl_a;
    /* verilator lint_on UNUSED */

    always @(posedge clk) begin
        if (rst) begin
            st      <= ST_IDLE;
            arvalid <= 1'b0;
            a_calc  <= 1'b0;
            a_done  <= 1'b0;
            r_done  <= 1'b0;
            q_v     <= 1'b0;
            wv      <= 1'b0;
            c_we    <= 8'h00;
        end else begin
            // ---------------- R capture (always accepted)
            q_v <= 1'b0;
            if (st == ST_RUN && rvalid && !r_done) begin
                q_v <= 1'b1;
                q_d <= rdata;
                q_a <= r_addr;
                if (r_left == 9'd0) begin
                    r_left    <= nwords - 9'd1;
                    r_addr    <= r_rowaddr + 13'd320;
                    r_rowaddr <= r_rowaddr + 13'd320;
                    r_rows    <= r_rows - 5'd1;
                    if (r_rows == 5'd1)
                        r_done <= 1'b1;
                end else begin
                    r_left <= r_left - 9'd1;
                    r_addr <= r_addr + 13'd1;
                end
            end
            // ---------------- write stage
            wv      <= q_v;
            w_addr  <= q_a[12:1];
            c_wdata <= {q_d, q_d};
            c_we    <= !q_v ? 8'h00 :
                       q_a[0] ? {pen, 4'b0000} : {4'b0000, pen};

            case (st)
                ST_IDLE: begin
                    if (start) begin
                        x      <= {w1[10:2], 2'b00};
                        y      <= w1[21:11];
                        w      <= {w2[10:2], 2'b00};
                        h      <= w2[21:11];
                        src    <= {w3[31:3], 3'b000};
                        stride <= {w4[31:3], 3'b000};
                        key    <= w5[15:0];
                        ck     <= w0[24];
                        st     <= ST_PREP;
                    end
                end
                ST_PREP: begin
                    mb     <= row0[9:0];
                    acc    <= src;
                    mm     <= stride;
                    nwords <= ncols[10:2];
                    nrows  <= ye[4:0] - ys[4:0] + 5'd1;   // ys, ye in the same strip
                    rbase0 <= {1'b0, ys[3:0], 8'd0} + {3'b000, ys[3:0], 6'd0} + {4'd0, x[10:2]};
                    st     <= ST_MUL;
                end
                ST_MUL: begin
                    if (mb != 10'd0) begin
                        if (mb[0])
                            acc <= acc + mm;
                        mm <= {mm[30:0], 1'b0};
                        mb <= {1'b0, mb[9:1]};
                    end else begin
                        st <= ST_WAIT;
                    end
                end
                ST_WAIT: begin
                    if (go_ok) begin
                        a_addr    <= acc;
                        a_row     <= acc;
                        a_left    <= nwords;
                        a_rows    <= nrows;
                        a_calc    <= 1'b1;
                        a_done    <= 1'b0;
                        r_left    <= nwords - 9'd1;
                        r_rows    <= nrows;
                        r_addr    <= rbase0;
                        r_rowaddr <= rbase0;
                        r_done    <= 1'b0;
                        st        <= ST_RUN;
                    end
                end
                default: begin // ST_RUN
                    if (a_calc) begin
                        arvalid <= 1'b1;
                        araddr  <= a_addr;
                        arlen   <= bl[3:0] - 4'd1;
                        a_bl    <= bl[4:0];
                        a_calc  <= 1'b0;
                    end else if (arvalid && arready) begin
                        arvalid <= 1'b0;
                        if ({4'd0, a_bl} == a_left) begin
                            // row finished
                            if (a_rows == 5'd1) begin
                                a_done <= 1'b1;
                            end else begin
                                a_rows <= a_rows - 5'd1;
                                a_row  <= a_row + stride;
                                a_addr <= a_row + stride;
                                a_left <= nwords;
                                a_calc <= 1'b1;
                            end
                        end else begin
                            a_addr <= a_addr + {24'd0, a_bl, 3'b000};
                            a_left <= a_left - {4'd0, a_bl};
                            a_calc <= 1'b1;
                        end
                    end
                    if (a_done && r_done && !q_v && !wv)
                        st <= ST_IDLE;
                end
            endcase
        end
    end
endmodule
