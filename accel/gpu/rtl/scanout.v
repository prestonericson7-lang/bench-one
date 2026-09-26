// scanout.v -- HP0 frame reader + async FIFO + video timing + TMDS + OSERDES (SPEC 8, 9).
//
//   core domain (clk):      video_reader  --{sof,data64}-->  async_fifo (65 x 2**FIFO_AW)
//   pixel domain (clk_pix): async_fifo --> video_pixel (timing, SOF resync, test pattern,
//                           RGB565->888) --> 3 x tmds_encoder --> 4 x video_serializer
//   serial (clk_ser = 5 x clk_pix, DDR): OSERDESE2 master/slave 10:1 -> OBUFDS pads
//
// Lanes: [0] = blue + {vsync,hsync} as {C1,C0}, [1] = green, [2] = red, [3] = clock
// (10'b0000011111, bit 0 first).  Channels 1/2 send control token 00 during blanking.
//
// Clock domain crossings (all other logic is single-domain):
//   * async_fifo: gray pointers, 2-FF ASYNC_REG synchronisers in both directions.
//   * scanout_en (core level) -> 2-FF ASYNC_REG synchroniser in video_pixel; applied at the
//     start of the next vertical blanking.
//   * vs_toggle (pixel) -> 2-FF ASYNC_REG synchroniser + edge detect -> vsync_count (core).
//   Required XDC (owned by whoever writes the constraints; checked in the Vivado 2026.1 OOC run
//   out/scanout/vivado/scanout_ooc.tcl at core 6.723 / pixel 13.445 / serial 2.689 ns):
//     set_clock_groups -asynchronous -group [get_clocks -of [get_pins <mmcm>/CLKOUT0]] \
//                     -group [get_clocks -of [get_pins {<mmcm>/CLKOUT1 <mmcm>/CLKOUT2}]]
//     set_bus_skew -from [get_cells <scan>/u_fifo/wgray_reg[*]] -to [get_cells <scan>/u_fifo/wgray_s1_reg[*]] 6.0
//     set_bus_skew -from [get_cells <scan>/u_fifo/rgray_reg[*]] -to [get_cells <scan>/u_fifo/rgray_s1_reg[*]] 6.0
//   set_bus_skew is still analysed under set_clock_groups (report_bus_skew); all 11 bits exist
//   as gray_reg cells because async_fifo marks them KEEP.  The single-bit syncs need nothing.
//   clk_pix and clk_ser stay related (same MMCM) -- do not put them in different groups.
//
// OSERDES reset: rst_pix re-registered in clk_pix (CLKDIV) and stretched by 4 cycles; one reset
// register for all four lanes, so they are released in the same CLKDIV cycle.
//
// The timing parameters default to 1280x720p60 CEA (SPEC 8); smaller values are only for fast
// simulation.  Frame = H_ACTIVE*V_ACTIVE/4 words (must be a multiple of 16), read contiguously
// from the buffer base (fb address low 7 bits are ignored = forced to 128-byte alignment).
module scanout #(
    parameter H_ACTIVE = 1280,
    parameter H_FP     = 110,
    parameter H_SYNC   = 40,
    parameter H_BP     = 220,
    parameter V_ACTIVE = 720,
    parameter V_FP     = 5,
    parameter V_SYNC   = 5,
    parameter V_BP     = 20,
    parameter FIFO_AW  = 10                      // FIFO depth 2**FIFO_AW words (1024 default)
) (
    input  wire        clk, rst,                 // core domain
    input  wire        clk_pix, clk_ser, rst_pix,// pixel domain (rst_pix sync to clk_pix)
    input  wire [31:0] fb0_addr, fb1_addr,
    input  wire        scanout_en,               // core domain level
    input  wire        swap_req, output wire swap_done, output wire front_idx,
    output wire [31:0] frame_count, output wire [31:0] vsync_count,   // core domain
    output wire        swap_pending,
    // AXI3 read master -> S_AXI_HP0 (64-bit), core domain
    output wire [31:0] rd_araddr, output wire [3:0] rd_arlen, output wire rd_arvalid, input wire rd_arready,
    input  wire [63:0] rd_rdata, input wire [1:0] rd_rresp, input wire rd_rlast, input wire rd_rvalid,
    output wire rd_rready,
    output wire [31:0] axi_err_cnt,
    // TMDS pads: [0]=data0 blue, [1]=data1 green, [2]=data2 red, [3]=clock
    output wire [3:0]  tmds_p, tmds_n
);
    localparam FRAME_WORDS = (H_ACTIVE * V_ACTIVE) / 4;

    // ---------------- core domain: reader ----------------
    wire             fifo_wr;
    wire [64:0]      fifo_din;
    wire [FIFO_AW:0] fifo_count;
    wire             fifo_full;

    video_reader #(
        .FRAME_WORDS (FRAME_WORDS),
        .FIFO_AW     (FIFO_AW)
    ) u_reader (
        .clk          (clk),
        .rst          (rst),
        .fb0_addr     (fb0_addr),
        .fb1_addr     (fb1_addr),
        .swap_req     (swap_req),
        .swap_done    (swap_done),
        .front_idx    (front_idx),
        .frame_count  (frame_count),
        .swap_pending (swap_pending),
        .rd_araddr    (rd_araddr),
        .rd_arlen     (rd_arlen),
        .rd_arvalid   (rd_arvalid),
        .rd_arready   (rd_arready),
        .rd_rdata     (rd_rdata),
        .rd_rresp     (rd_rresp),
        .rd_rlast     (rd_rlast),
        .rd_rvalid    (rd_rvalid),
        .rd_rready    (rd_rready),
        .axi_err_cnt  (axi_err_cnt),
        .fifo_wr      (fifo_wr),
        .fifo_din     (fifo_din),
        .fifo_count   (fifo_count)
    );

    // ---------------- CDC FIFO ----------------
    wire        f_valid;
    wire [64:0] f_data;
    wire        f_pop;

    async_fifo #(
        .W  (65),
        .AW (FIFO_AW)
    ) u_fifo (
        .wclk   (clk),
        .wrst   (rst),
        .wr_en  (fifo_wr),
        .wdata  (fifo_din),
        .wfull  (fifo_full),
        .wcount (fifo_count),
        .rclk   (clk_pix),
        .rrst   (rst_pix),
        .rd_en  (f_pop),
        .rdata  (f_data),
        .rvalid (f_valid)
    );

    // ---------------- pixel domain ----------------
    wire       v_de, v_hs, v_vs, vs_toggle;
    wire [7:0] v_r, v_g, v_b;

    video_pixel #(
        .H_ACTIVE (H_ACTIVE), .H_FP (H_FP), .H_SYNC (H_SYNC), .H_BP (H_BP),
        .V_ACTIVE (V_ACTIVE), .V_FP (V_FP), .V_SYNC (V_SYNC), .V_BP (V_BP)
    ) u_pix (
        .clk_pix    (clk_pix),
        .rst_pix    (rst_pix),
        .scanout_en (scanout_en),
        .f_valid    (f_valid),
        .f_data     (f_data),
        .f_pop      (f_pop),
        .de         (v_de),
        .hsync      (v_hs),
        .vsync      (v_vs),
        .r          (v_r),
        .g          (v_g),
        .b          (v_b),
        .vs_toggle  (vs_toggle)
    );

    wire [9:0] sym0, sym1, sym2;
    tmds_encoder u_enc0 (.clk(clk_pix), .rst(rst_pix), .d(v_b), .c0(v_hs), .c1(v_vs), .de(v_de), .q(sym0));
    tmds_encoder u_enc1 (.clk(clk_pix), .rst(rst_pix), .d(v_g), .c0(1'b0), .c1(1'b0), .de(v_de), .q(sym1));
    tmds_encoder u_enc2 (.clk(clk_pix), .rst(rst_pix), .d(v_r), .c0(1'b0), .c1(1'b0), .de(v_de), .q(sym2));

    reg [3:0] oser_rst_sr = 4'hF;      // INIT = 1: OSERDES held in reset from configuration
    always @(posedge clk_pix) begin
        if (rst_pix) oser_rst_sr <= 4'hF;
        else         oser_rst_sr <= {oser_rst_sr[2:0], 1'b0};
    end
    wire oser_rst = oser_rst_sr[3];

    video_serializer u_ser0 (.clk_pix(clk_pix), .clk_ser(clk_ser), .rst(oser_rst), .d(sym0),
                             .pad_p(tmds_p[0]), .pad_n(tmds_n[0]));
    video_serializer u_ser1 (.clk_pix(clk_pix), .clk_ser(clk_ser), .rst(oser_rst), .d(sym1),
                             .pad_p(tmds_p[1]), .pad_n(tmds_n[1]));
    video_serializer u_ser2 (.clk_pix(clk_pix), .clk_ser(clk_ser), .rst(oser_rst), .d(sym2),
                             .pad_p(tmds_p[2]), .pad_n(tmds_n[2]));
    video_serializer u_serc (.clk_pix(clk_pix), .clk_ser(clk_ser), .rst(oser_rst), .d(10'b0000011111),
                             .pad_p(tmds_p[3]), .pad_n(tmds_n[3]));

    // ---------------- vsync_count (pixel toggle -> core) ----------------
    (* ASYNC_REG = "TRUE" *) reg vt_s1;
    (* ASYNC_REG = "TRUE" *) reg vt_s2;
    reg        vt_s3;
    reg [31:0] vsync_cnt_r;
    always @(posedge clk) begin
        if (rst) begin
            vt_s1 <= 1'b0; vt_s2 <= 1'b0; vt_s3 <= 1'b0;
            vsync_cnt_r <= 32'd0;
        end else begin
            vt_s1 <= vs_toggle;
            vt_s2 <= vt_s1;
            vt_s3 <= vt_s2;
            if (vt_s2 ^ vt_s3)
                vsync_cnt_r <= vsync_cnt_r + 32'd1;
        end
    end
    assign vsync_count = vsync_cnt_r;

    /* verilator lint_off UNUSED */
    wire unused_full = fifo_full;   // cannot happen: the reader reserves room before each AR
    /* verilator lint_on UNUSED */
endmodule
