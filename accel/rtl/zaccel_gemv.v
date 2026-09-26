/* ===========================================================================================
 *  zaccel_gemv.v -- the Zynq's matrix engine for the Orange Pi (contract: accel/SPEC.md §1)
 * ===========================================================================================
 *
 *  AXI-Stream in, AXI-Stream out, fed and drained by an AXI DMA from the Zynq's DDR3.
 *
 *    in : header | activations (nb vectors) | weight rows
 *    out: y[r][v] as int32, two per beat | trailer (cycles, rows, 0x5A45) with TLAST
 *
 *  Every weight beat is multiplied against up to MAX_BATCH activation vectors at once, so one pass
 *  over the weights -- the expensive part, they live in DDR3 -- serves a whole batch:
 *    mode 0 (int4 weights): 16 weights x nb vectors per clock
 *    mode 1 (int8 weights):  8 weights x nb vectors per clock
 *
 *  PIPELINE (all stages advance together on `pe`)
 *    S1  beat registered, activation words read from block RAM (one address for all memories)
 *    S2  products, one per lane per vector, masked to the row width and the batch
 *    S3  sums of four lanes
 *    S4  sum of the beat
 *    S5  accumulate; the last beat of a row hands the row's nb results to the serializer
 *  `pe` drops only when a finished row is waiting for a serializer that is still busy, which stalls
 *  the input through s_axis_tready and never loses or repeats a result.
 * ===========================================================================================
 */
`default_nettype none

module zaccel_gemv #(
    parameter integer MAX_COLS  = 4096,
    parameter integer MAX_BATCH = 8
) (
    input  wire        aclk,
    input  wire        aresetn,

    input  wire [63:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,     /* ignored: the engine counts beats */

    output wire [63:0] m_axis_tdata,
    output wire [7:0]  m_axis_tkeep,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast
);

    localparam integer DEPTH = MAX_COLS / 16;             /* 64-bit words per bank per vector */
    localparam integer MAW   = $clog2(DEPTH);             /* bank address width               */
    localparam integer BW    = $clog2(MAX_COLS / 8) + 1;  /* beat counters                    */

    localparam [1:0] S_HDR = 2'd0, S_ACT = 2'd1, S_W = 2'd2, S_DRAIN = 2'd3;

    reg  [1:0]  state;
    reg         mode;          /* 0 = int4 weights, 1 = int8 weights */
    reg  [3:0]  nb;            /* 1..MAX_BATCH                       */
    reg  [12:0] cols;          /* 1..MAX_COLS                        */
    reg  [15:0] rows;          /* 1..65535                           */
    reg  [BW-1:0] act_beats;   /* ceil(cols/8)                       */
    reg  [BW-1:0] row_beats;   /* ceil(cols/16) or ceil(cols/8)      */

    reg  [3:0]  act_v;         /* vector being loaded                */
    reg  [BW-1:0] act_j;       /* activation beat within the vector  */
    reg  [BW-1:0] wb;          /* weight beat within the row         */
    reg  [15:0] wrow;          /* row being received                 */
    reg  [31:0] cycles;

    wire        pe;            /* pipeline enable                    */

    /* ---------------------------------------------------------------- header decode */
    wire [15:0] h_magic = s_axis_tdata[15:0];
    wire [3:0]  h_mode  = s_axis_tdata[19:16];
    wire [3:0]  h_nbm1  = s_axis_tdata[23:20];
    wire [15:0] h_cols  = s_axis_tdata[47:32];
    wire [15:0] h_rows  = s_axis_tdata[63:48];
    wire        h_ok    = (h_magic == 16'h5A41) && (h_mode <= 4'd1) && ({1'b0, h_nbm1} < MAX_BATCH)
                        && (h_cols != 16'd0) && (h_cols <= MAX_COLS) && (h_rows != 16'd0);

    assign s_axis_tready = (state == S_HDR) || (state == S_ACT) || ((state == S_W) && pe);
    wire   acc_in = s_axis_tvalid && s_axis_tready;

    wire act_last_beat = (act_j == act_beats - 1'b1);
    wire w_last_beat   = (wb == row_beats - 1'b1);
    wire w_last_row    = (wrow == rows - 1'b1);

    /* ---------------------------------------------------------------- activation memories
     * Per vector, two banks of 64-bit words: activation beat j lives in bank j[0] at word j/2.
     * An int4 weight beat b needs activations 16b..16b+15 = both banks at word b; an int8 weight
     * beat b needs activations 8b..8b+7 = bank b[0] at word b/2. Either way every memory is read at
     * ONE address per clock, which is what a block RAM does. */
    wire             act_we  = (state == S_ACT) && acc_in;
    wire [MAW-1:0]   wr_addr = act_j[MAW:1];
    wire [MAW-1:0]   rd_addr = mode ? wb[MAW:1] : wb[MAW-1:0];

    wire [63:0] rd_e [0:MAX_BATCH-1];
    wire [63:0] rd_o [0:MAX_BATCH-1];

    genvar gv, gl;
    generate
        for (gv = 0; gv < MAX_BATCH; gv = gv + 1) begin : vec
            (* ram_style = "block" *) reg [63:0] me [0:DEPTH-1];
            (* ram_style = "block" *) reg [63:0] mo [0:DEPTH-1];
            reg [63:0] re, ro;
            always @(posedge aclk) begin
                if (act_we && (act_v == gv) && !act_j[0]) me[wr_addr] <= s_axis_tdata;
                if (act_we && (act_v == gv) &&  act_j[0]) mo[wr_addr] <= s_axis_tdata;
                if (pe) begin
                    re <= me[rd_addr];
                    ro <= mo[rd_addr];
                end
            end
            assign rd_e[gv] = re;
            assign rd_o[gv] = ro;
        end
    endgenerate

    /* ---------------------------------------------------------------- S1 */
    reg        s1_valid, s1_last, s1_b0;
    reg [63:0] s1_w;
    reg [12:0] s1_col;         /* first column this beat covers */

    wire       w_take = (state == S_W) && acc_in;

    always @(posedge aclk) begin
        if (!aresetn) begin
            s1_valid <= 1'b0;
        end else if (pe) begin
            s1_valid <= w_take;
            s1_last  <= w_take && w_last_beat;
            s1_w     <= s_axis_tdata;
            s1_b0    <= wb[0];
            s1_col   <= mode ? {wb, 3'b000} : {wb, 4'b0000};
        end
    end

    /* ---------------------------------------------------------------- S2: products */
    reg signed [15:0] p [0:MAX_BATCH-1][0:15];
    reg s2_valid, s2_last;

    generate
        for (gv = 0; gv < MAX_BATCH; gv = gv + 1) begin : pv
            for (gl = 0; gl < 16; gl = gl + 1) begin : pl
                /* lanes 0..7 serve both modes; lanes 8..15 only int4 (mode 0) */
                wire [7:0] w4 = {{4{s1_w[gl*4 + 3]}}, s1_w[gl*4 +: 4]};
                wire [7:0] w8, a8;
                wire       lane_mode_ok;
                if (gl < 8) begin : lo
                    assign w8 = mode ? s1_w[gl*8 +: 8] : w4;
                    assign a8 = mode ? (s1_b0 ? rd_o[gv][gl*8 +: 8] : rd_e[gv][gl*8 +: 8]) : rd_e[gv][gl*8 +: 8];
                    assign lane_mode_ok = 1'b1;
                end else begin : hi
                    assign w8 = w4;
                    assign a8 = rd_o[gv][(gl-8)*8 +: 8];
                    assign lane_mode_ok = !mode;
                end
                wire lane_ok = ({1'b0, s1_col} + gl < {1'b0, cols}) && (gv < nb) && lane_mode_ok;
                always @(posedge aclk)
                    if (pe) p[gv][gl] <= lane_ok ? ($signed(w8) * $signed(a8)) : 16'sd0;
            end
        end
    endgenerate

    always @(posedge aclk) begin
        if (!aresetn) s2_valid <= 1'b0;
        else if (pe) begin s2_valid <= s1_valid; s2_last <= s1_last; end
    end

    /* ---------------------------------------------------------------- S3: sums of four */
    reg signed [17:0] q [0:MAX_BATCH-1][0:3];
    reg s3_valid, s3_last;
    generate
        for (gv = 0; gv < MAX_BATCH; gv = gv + 1) begin : qv
            for (gl = 0; gl < 4; gl = gl + 1) begin : ql
                always @(posedge aclk)
                    if (pe) q[gv][gl] <= p[gv][gl*4] + p[gv][gl*4+1] + p[gv][gl*4+2] + p[gv][gl*4+3];
            end
        end
    endgenerate
    always @(posedge aclk) begin
        if (!aresetn) s3_valid <= 1'b0;
        else if (pe) begin s3_valid <= s2_valid; s3_last <= s2_last; end
    end

    /* ---------------------------------------------------------------- S4: sum of the beat */
    reg signed [19:0] bs [0:MAX_BATCH-1];
    reg s4_valid, s4_last;
    generate
        for (gv = 0; gv < MAX_BATCH; gv = gv + 1) begin : bv
            always @(posedge aclk)
                if (pe) bs[gv] <= q[gv][0] + q[gv][1] + q[gv][2] + q[gv][3];
        end
    endgenerate
    always @(posedge aclk) begin
        if (!aresetn) s4_valid <= 1'b0;
        else if (pe) begin s4_valid <= s3_valid; s4_last <= s3_last; end
    end

    /* ---------------------------------------------------------------- S5: accumulate */
    reg signed [31:0] acc     [0:MAX_BATCH-1];
    reg signed [31:0] row_val [0:MAX_BATCH-1];
    reg               row_out_valid;
    generate
        for (gv = 0; gv < MAX_BATCH; gv = gv + 1) begin : av
            wire signed [31:0] sum = acc[gv] + {{12{bs[gv][19]}}, bs[gv]};
            always @(posedge aclk) begin
                if (!aresetn) acc[gv] <= 32'sd0;
                else if (pe && s4_valid) begin
                    if (s4_last) begin row_val[gv] <= sum; acc[gv] <= 32'sd0; end
                    else acc[gv] <= sum;
                end
            end
        end
    endgenerate

    /* ---------------------------------------------------------------- serializer + packer */
    reg  [3:0]  ser_cnt;                   /* values left to emit */
    reg  [2:0]  ser_idx;
    reg  [31:0] ser_val [0:MAX_BATCH-1];
    reg  [31:0] half;
    reg         have_half;

    wire        fifo_full;
    wire        emit     = (ser_cnt != 4'd0) && (!have_half || !fifo_full);
    wire        ser_idle = (ser_cnt == 4'd0) || ((ser_cnt == 4'd1) && emit);
    wire        ser_take = row_out_valid && ser_idle;
    assign      pe       = !row_out_valid || ser_idle;

    /* trailer / flush, driven by the S_DRAIN state */
    wire pipe_empty = !s1_valid && !s2_valid && !s3_valid && !s4_valid && !row_out_valid && (ser_cnt == 4'd0);
    wire flush_half = (state == S_DRAIN) && pipe_empty && have_half && !fifo_full;
    wire push_trl   = (state == S_DRAIN) && pipe_empty && !have_half && !fifo_full;

    wire        push_val = emit && have_half;
    wire        fifo_wr  = push_val || flush_half || push_trl;
    wire [64:0] fifo_din = push_trl   ? {1'b1, 16'h5A45, rows, cycles} :
                           flush_half ? {1'b0, 32'd0, half} :
                                        {1'b0, ser_val[ser_idx], half};

    integer k;
    always @(posedge aclk) begin
        if (!aresetn) begin
            row_out_valid <= 1'b0;
            ser_cnt   <= 4'd0;
            ser_idx   <= 3'd0;
            have_half <= 1'b0;
        end else begin
            if (pe) row_out_valid <= s4_valid && s4_last;

            if (emit) begin
                if (!have_half) half <= ser_val[ser_idx];
                have_half <= !have_half;
                ser_idx   <= ser_idx + 1'b1;
                ser_cnt   <= ser_cnt - 1'b1;
            end
            if (flush_half) have_half <= 1'b0;

            if (ser_take) begin
                for (k = 0; k < MAX_BATCH; k = k + 1) ser_val[k] <= row_val[k];
                ser_cnt  <= nb;
                ser_idx  <= 3'd0;
            end
        end
    end

    /* ---------------------------------------------------------------- output FIFO */
    localparam integer FD = 16;
    reg [64:0] fifo [0:FD-1];
    reg [4:0]  wp, rp;
    wire       fifo_empty = (wp == rp);
    assign     fifo_full  = (wp[3:0] == rp[3:0]) && (wp[4] != rp[4]);
    wire       fifo_rd    = !fifo_empty && m_axis_tready;
    always @(posedge aclk) begin
        if (!aresetn) begin wp <= 5'd0; rp <= 5'd0; end
        else begin
            if (fifo_wr) begin fifo[wp[3:0]] <= fifo_din; wp <= wp + 1'b1; end
            if (fifo_rd) rp <= rp + 1'b1;
        end
    end
    wire [64:0] fifo_q = fifo[rp[3:0]];
    assign m_axis_tdata  = fifo_q[63:0];
    assign m_axis_tlast  = fifo_q[64];
    assign m_axis_tkeep  = 8'hFF;
    assign m_axis_tvalid = !fifo_empty;

    /* ---------------------------------------------------------------- input control */
    always @(posedge aclk) begin
        if (!aresetn) begin
            state <= S_HDR;
            act_v <= 4'd0; act_j <= {BW{1'b0}}; wb <= {BW{1'b0}}; wrow <= 16'd0; cycles <= 32'd0;
        end else begin
            if ((state == S_ACT) || (state == S_W)) cycles <= cycles + 1'b1;
            case (state)
            S_HDR: if (acc_in && h_ok) begin
                mode      <= h_mode[0];
                nb        <= h_nbm1 + 4'd1;
                cols      <= h_cols[12:0];
                rows      <= h_rows;
                act_beats <= (h_cols + 16'd7) >> 3;
                row_beats <= h_mode[0] ? ((h_cols + 16'd7) >> 3) : ((h_cols + 16'd15) >> 4);
                act_v <= 4'd0; act_j <= {BW{1'b0}}; wb <= {BW{1'b0}}; wrow <= 16'd0;
                cycles <= 32'd0;
                state <= S_ACT;
            end
            S_ACT: if (acc_in) begin
                if (act_last_beat) begin
                    act_j <= {BW{1'b0}};
                    if (act_v == nb - 1'b1) state <= S_W;
                    else act_v <= act_v + 1'b1;
                end else act_j <= act_j + 1'b1;
            end
            S_W: if (acc_in) begin
                if (w_last_beat) begin
                    wb <= {BW{1'b0}};
                    if (w_last_row) state <= S_DRAIN;
                    else wrow <= wrow + 1'b1;
                end else wb <= wb + 1'b1;
            end
            S_DRAIN: if (push_trl) state <= S_HDR;
            endcase
        end
    end

endmodule

`default_nettype wire
