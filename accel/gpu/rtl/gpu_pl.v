// ---------------------------------------------------------------------------------------------
// gpu_pl -- the FPGA-GPU's programmable-logic part (SPEC sections 2, 3, 7, 9), without a PS7
//
// Everything of the GPU that lives in the fabric: clkgen (MMCM from the 50 MHz U18 clock), the
// Teensy bus receiver + FIFO, the GP0 register slave + PS command FIFO, the render core and the
// scanout. The four PS7 AXI ports it uses are plain ports here, so the same module sits under
//   * rtl/gpu_top.v   -- the standalone GPU board top (fpga/build.tcl, its own ps7_bd), and
//   * hardware/pz7020-starlite/vivado/system_top.v -- the one PZ7020 system bitstream (platform
//     + GPU + matrix engine), where the GP window reaches this slave through the GP0 interconnect.
// This split is the only change: the logic, the constants and the rendering are those of the
// previous single-file gpu_top.
//
//   aclk  = core clock (148.75 MHz): the ACLK of all four ports, driven out to the PS7
//   gp_*  = AXI3 slave (32-bit, 12-bit IDs): PS M_AXI_GP0 -> axi_gp_regs, window 0x43C00000/4K
//   hp0_* = AXI3 master (64-bit, 6-bit IDs) -> S_AXI_HP0: scanout reads   (write channel idle)
//   hp1_* = AXI3 master                     -> S_AXI_HP1: strip writes    (read channel idle)
//   hp2_* = AXI3 master                     -> S_AXI_HP2: sprite reads    (write channel idle)
//
// Constant AXI fields (SPEC 9) are tied off here: size 3 (8 bytes), INCR, cache 0011, prot 0,
// lock 0, qos 0, IDs 0, WSTRB FF, WID 0. Unused channels are idle: HP1 read channel
// (ARVALID 0, RREADY 1), HP0/HP2 write channels (AWVALID 0, WVALID 0, BREADY 1).
//
// RENDER CORE MODULE: the core is instantiated as `core_top` (engineer B), which has exactly the
// rtl/INTERFACES.md gpu_core ports PLUS the SPEC 13.1 return-capture ports (ret_addr, ret_enable,
// ret_ack, ret_full, ret_capturing, ret_frame, last_frame_no). rtl/gpu_core.v is core_top with
// those ports tied off (RET never enabled). RET_* registers (gpu_proto.h 0x04C..0x05C) live in
// axi_gp_regs.
//
// SOFT_RESET (CONTROL bit 0, 1-cycle pulse from axi_gp_regs) resets par_rx and both FIFOs in the
// SAME cycle as gpu_core sees soft_reset (combinational OR with rst), so the collector can never
// pop a stale pre-reset word. It does not touch clkgen, axi_gp_regs or scanout.
//
// LEDs: led[0] heartbeat (toggles every 0.5 s -> ~1 Hz blink), led[1] toggles every 30
// FRAME_COUNT increments. hdmi_out_en = 1. hdmi_hpd is 2-FF synchronised into the core domain.
// ---------------------------------------------------------------------------------------------
module gpu_pl (
    // ---- PL pins (SPEC section 2) ----
    input  wire        clk50,
    output wire [1:0]  led,
    output wire        hdmi_d0_p,
    output wire        hdmi_d0_n,
    output wire        hdmi_d1_p,
    output wire        hdmi_d1_n,
    output wire        hdmi_d2_p,
    output wire        hdmi_d2_n,
    output wire        hdmi_clk_p,
    output wire        hdmi_clk_n,
    output wire        hdmi_out_en,
    input  wire        hdmi_hpd,
    input  wire [15:0] tb_d,
    input  wire        tb_sor,
    input  wire        tb_strobe,
    output wire        tb_busy,
    // ---- core clock = ACLK of M_AXI_GP0 (GPU window) and S_AXI_HP0/1/2 ----
    output wire        aclk,
    // ---- GP0 slave (PS master -> axi_gp_regs) ----
    input  wire [31:0] gp_araddr,
    input  wire [1:0]  gp_arburst,
    input  wire [3:0]  gp_arcache,
    input  wire [11:0] gp_arid,
    input  wire [3:0]  gp_arlen,
    input  wire [1:0]  gp_arlock,
    input  wire [2:0]  gp_arprot,
    input  wire [3:0]  gp_arqos,
    output wire        gp_arready,
    input  wire [2:0]  gp_arsize,
    input  wire        gp_arvalid,
    input  wire [31:0] gp_awaddr,
    input  wire [1:0]  gp_awburst,
    input  wire [3:0]  gp_awcache,
    input  wire [11:0] gp_awid,
    input  wire [3:0]  gp_awlen,
    input  wire [1:0]  gp_awlock,
    input  wire [2:0]  gp_awprot,
    input  wire [3:0]  gp_awqos,
    output wire        gp_awready,
    input  wire [2:0]  gp_awsize,
    input  wire        gp_awvalid,
    output wire [11:0] gp_bid,
    input  wire        gp_bready,
    output wire [1:0]  gp_bresp,
    output wire        gp_bvalid,
    output wire [31:0] gp_rdata,
    output wire [11:0] gp_rid,
    output wire        gp_rlast,
    input  wire        gp_rready,
    output wire [1:0]  gp_rresp,
    output wire        gp_rvalid,
    input  wire [31:0] gp_wdata,
    input  wire [11:0] gp_wid,
    input  wire        gp_wlast,
    output wire        gp_wready,
    input  wire [3:0]  gp_wstrb,
    input  wire        gp_wvalid,
    // ---- HP0 master (scanout reads) ----
    output wire [31:0] hp0_araddr,
    output wire [1:0]  hp0_arburst,
    output wire [3:0]  hp0_arcache,
    output wire [5:0]  hp0_arid,
    output wire [3:0]  hp0_arlen,
    output wire [1:0]  hp0_arlock,
    output wire [2:0]  hp0_arprot,
    output wire [3:0]  hp0_arqos,
    input  wire        hp0_arready,
    output wire [2:0]  hp0_arsize,
    output wire        hp0_arvalid,
    output wire [31:0] hp0_awaddr,
    output wire [1:0]  hp0_awburst,
    output wire [3:0]  hp0_awcache,
    output wire [5:0]  hp0_awid,
    output wire [3:0]  hp0_awlen,
    output wire [1:0]  hp0_awlock,
    output wire [2:0]  hp0_awprot,
    output wire [3:0]  hp0_awqos,
    input  wire        hp0_awready,
    output wire [2:0]  hp0_awsize,
    output wire        hp0_awvalid,
    input  wire [5:0]  hp0_bid,
    output wire        hp0_bready,
    input  wire [1:0]  hp0_bresp,
    input  wire        hp0_bvalid,
    input  wire [63:0] hp0_rdata,
    input  wire [5:0]  hp0_rid,
    input  wire        hp0_rlast,
    output wire        hp0_rready,
    input  wire [1:0]  hp0_rresp,
    input  wire        hp0_rvalid,
    output wire [63:0] hp0_wdata,
    output wire [5:0]  hp0_wid,
    output wire        hp0_wlast,
    input  wire        hp0_wready,
    output wire [7:0]  hp0_wstrb,
    output wire        hp0_wvalid,
    // ---- HP1 master (strip writes) ----
    output wire [31:0] hp1_araddr,
    output wire [1:0]  hp1_arburst,
    output wire [3:0]  hp1_arcache,
    output wire [5:0]  hp1_arid,
    output wire [3:0]  hp1_arlen,
    output wire [1:0]  hp1_arlock,
    output wire [2:0]  hp1_arprot,
    output wire [3:0]  hp1_arqos,
    input  wire        hp1_arready,
    output wire [2:0]  hp1_arsize,
    output wire        hp1_arvalid,
    output wire [31:0] hp1_awaddr,
    output wire [1:0]  hp1_awburst,
    output wire [3:0]  hp1_awcache,
    output wire [5:0]  hp1_awid,
    output wire [3:0]  hp1_awlen,
    output wire [1:0]  hp1_awlock,
    output wire [2:0]  hp1_awprot,
    output wire [3:0]  hp1_awqos,
    input  wire        hp1_awready,
    output wire [2:0]  hp1_awsize,
    output wire        hp1_awvalid,
    input  wire [5:0]  hp1_bid,
    output wire        hp1_bready,
    input  wire [1:0]  hp1_bresp,
    input  wire        hp1_bvalid,
    input  wire [63:0] hp1_rdata,
    input  wire [5:0]  hp1_rid,
    input  wire        hp1_rlast,
    output wire        hp1_rready,
    input  wire [1:0]  hp1_rresp,
    input  wire        hp1_rvalid,
    output wire [63:0] hp1_wdata,
    output wire [5:0]  hp1_wid,
    output wire        hp1_wlast,
    input  wire        hp1_wready,
    output wire [7:0]  hp1_wstrb,
    output wire        hp1_wvalid,
    // ---- HP2 master (sprite reads) ----
    output wire [31:0] hp2_araddr,
    output wire [1:0]  hp2_arburst,
    output wire [3:0]  hp2_arcache,
    output wire [5:0]  hp2_arid,
    output wire [3:0]  hp2_arlen,
    output wire [1:0]  hp2_arlock,
    output wire [2:0]  hp2_arprot,
    output wire [3:0]  hp2_arqos,
    input  wire        hp2_arready,
    output wire [2:0]  hp2_arsize,
    output wire        hp2_arvalid,
    output wire [31:0] hp2_awaddr,
    output wire [1:0]  hp2_awburst,
    output wire [3:0]  hp2_awcache,
    output wire [5:0]  hp2_awid,
    output wire [3:0]  hp2_awlen,
    output wire [1:0]  hp2_awlock,
    output wire [2:0]  hp2_awprot,
    output wire [3:0]  hp2_awqos,
    input  wire        hp2_awready,
    output wire [2:0]  hp2_awsize,
    output wire        hp2_awvalid,
    input  wire [5:0]  hp2_bid,
    output wire        hp2_bready,
    input  wire [1:0]  hp2_bresp,
    input  wire        hp2_bvalid,
    input  wire [63:0] hp2_rdata,
    input  wire [5:0]  hp2_rid,
    input  wire        hp2_rlast,
    output wire        hp2_rready,
    input  wire [1:0]  hp2_rresp,
    input  wire        hp2_rvalid,
    output wire [63:0] hp2_wdata,
    output wire [5:0]  hp2_wid,
    output wire        hp2_wlast,
    input  wire        hp2_wready,
    output wire [7:0]  hp2_wstrb,
    output wire        hp2_wvalid
);
    // =========================================================================================
    // Clocks and resets
    // =========================================================================================
    wire clk, clk_pix, clk_ser, mmcm_locked, rst, rst_pix;

    clkgen u_clkgen (
        .clk50   (clk50),
        .clk     (clk),
        .clk_pix (clk_pix),
        .clk_ser (clk_ser),
        .locked  (mmcm_locked),
        .rst     (rst),
        .rst_pix (rst_pix)
    );

    wire soft_reset;
    wire rst_io = rst | soft_reset;          // bus receiver + both command FIFOs

    // asynchronous status inputs -> core domain
    (* ASYNC_REG = "TRUE" *) reg [1:0] hpd_s    = 2'b00;
    (* ASYNC_REG = "TRUE" *) reg [1:0] locked_s = 2'b00;
    always @(posedge clk) begin
        hpd_s    <= {hpd_s[0], hdmi_hpd};
        locked_s <= {locked_s[0], mmcm_locked};
    end

    assign aclk = clk;

    // =========================================================================================
    // Constant AXI fields and idle channels of the HP masters (SPEC 9)
    // =========================================================================================
    // HP0: scanout reads, write channel idle
    assign hp0_arburst = 2'b01;   assign hp0_arcache = 4'b0011; assign hp0_arid    = 6'd0;
    assign hp0_arlock  = 2'b00;   assign hp0_arprot  = 3'b000;  assign hp0_arqos   = 4'b0000;
    assign hp0_arsize  = 3'd3;
    assign hp0_awaddr  = 32'd0;   assign hp0_awburst = 2'b01;   assign hp0_awcache = 4'b0011;
    assign hp0_awid    = 6'd0;    assign hp0_awlen   = 4'd0;    assign hp0_awlock  = 2'b00;
    assign hp0_awprot  = 3'b000;  assign hp0_awqos   = 4'b0000; assign hp0_awsize  = 3'd3;
    assign hp0_awvalid = 1'b0;    assign hp0_bready  = 1'b1;
    assign hp0_wdata   = 64'd0;   assign hp0_wid     = 6'd0;    assign hp0_wlast   = 1'b0;
    assign hp0_wstrb   = 8'hFF;   assign hp0_wvalid  = 1'b0;

    // HP1: strip writes, read channel idle
    assign hp1_araddr  = 32'd0;   assign hp1_arburst = 2'b01;   assign hp1_arcache = 4'b0011;
    assign hp1_arid    = 6'd0;    assign hp1_arlen   = 4'd0;    assign hp1_arlock  = 2'b00;
    assign hp1_arprot  = 3'b000;  assign hp1_arqos   = 4'b0000; assign hp1_arsize  = 3'd3;
    assign hp1_arvalid = 1'b0;    assign hp1_rready  = 1'b1;
    assign hp1_awburst = 2'b01;   assign hp1_awcache = 4'b0011; assign hp1_awid    = 6'd0;
    assign hp1_awlock  = 2'b00;   assign hp1_awprot  = 3'b000;  assign hp1_awqos   = 4'b0000;
    assign hp1_awsize  = 3'd3;
    assign hp1_wid     = 6'd0;    assign hp1_wstrb   = 8'hFF;

    // HP2: sprite reads, write channel idle
    assign hp2_arburst = 2'b01;   assign hp2_arcache = 4'b0011; assign hp2_arid    = 6'd0;
    assign hp2_arlock  = 2'b00;   assign hp2_arprot  = 3'b000;  assign hp2_arqos   = 4'b0000;
    assign hp2_arsize  = 3'd3;
    assign hp2_awaddr  = 32'd0;   assign hp2_awburst = 2'b01;   assign hp2_awcache = 4'b0011;
    assign hp2_awid    = 6'd0;    assign hp2_awlen   = 4'd0;    assign hp2_awlock  = 2'b00;
    assign hp2_awprot  = 3'b000;  assign hp2_awqos   = 4'b0000; assign hp2_awsize  = 3'd3;
    assign hp2_awvalid = 1'b0;    assign hp2_bready  = 1'b1;
    assign hp2_wdata   = 64'd0;   assign hp2_wid     = 6'd0;    assign hp2_wlast   = 1'b0;
    assign hp2_wstrb   = 8'hFF;   assign hp2_wvalid  = 1'b0;

    // =========================================================================================
    // Teensy bus receiver + Teensy command FIFO (33 x 1024)
    // =========================================================================================
    wire        t_wr, t_empty, t_rd;
    wire [32:0] t_din, t_dout;
    wire [10:0] t_free, t_level;
    wire [31:0] t_words;
    wire        teensy_active;
    wire        t_full_unused;

    par_rx u_par_rx (
        .clk        (clk),
        .rst        (rst_io),
        .pin_d      (tb_d),
        .pin_sor    (tb_sor),
        .pin_strobe (tb_strobe),
        .pin_busy   (tb_busy),
        .f_wr       (t_wr),
        .f_din      (t_din),
        .f_free     (t_free),
        .words_rx   (t_words),
        .active     (teensy_active)
    );

    sync_fifo #(.W(33), .AW(10)) u_tfifo (
        .clk   (clk),
        .rst   (rst_io),
        .wr    (t_wr),
        .din   (t_din),
        .rd    (t_rd),
        .dout  (t_dout),
        .empty (t_empty),
        .full  (t_full_unused),
        .count (t_level),
        .free  (t_free)
    );

    // =========================================================================================
    // GP0 registers + PS command FIFO (33 x 512)
    // =========================================================================================
    wire        pf_wr, p_empty, p_rd;
    wire [32:0] pf_din, p_dout;
    wire [9:0]  pf_free, p_count_unused;
    wire        p_full_unused;

    wire        src_teensy_en, src_ps_en, scanout_en;
    wire [31:0] fb0_addr, fb1_addr;
    wire [15:0] clear_color;

    wire        raster_busy, wait_teensy, wait_ps;
    wire [31:0] list_overflow_cnt, bad_record_cnt, render_cycles, prim_count, core_axi_err;
    wire [31:0] dropped_cnt;
    wire        swap_req, swap_done, front_idx, swap_pending;
    wire [31:0] frame_count, vsync_count, scan_axi_err;
    // SPEC 13.1 return capture
    wire [31:0] ret_addr, ret_frame, last_frame_no;
    wire        ret_enable, ret_ack, ret_full, ret_capturing;

    reg  [31:0] axi_err_sum;
    always @(posedge clk)
        axi_err_sum <= core_axi_err + scan_axi_err;

    sync_fifo #(.W(33), .AW(9)) u_pfifo (
        .clk   (clk),
        .rst   (rst_io),
        .wr    (pf_wr),
        .din   (pf_din),
        .rd    (p_rd),
        .dout  (p_dout),
        .empty (p_empty),
        .full  (p_full_unused),
        .count (p_count_unused),
        .free  (pf_free)
    );

    axi_gp_regs u_regs (
        .clk               (clk),
        .rst               (rst),
        .awaddr            (gp_awaddr),
        .awlen             (gp_awlen),
        .awsize            (gp_awsize),
        .awburst           (gp_awburst),
        .awid              (gp_awid),
        .awvalid           (gp_awvalid),
        .awready           (gp_awready),
        .wdata             (gp_wdata),
        .wstrb             (gp_wstrb),
        .wlast             (gp_wlast),
        .wid               (gp_wid),
        .wvalid            (gp_wvalid),
        .wready            (gp_wready),
        .bresp             (gp_bresp),
        .bid               (gp_bid),
        .bvalid            (gp_bvalid),
        .bready            (gp_bready),
        .araddr            (gp_araddr),
        .arlen             (gp_arlen),
        .arsize            (gp_arsize),
        .arburst           (gp_arburst),
        .arid              (gp_arid),
        .arvalid           (gp_arvalid),
        .arready           (gp_arready),
        .rdata             (gp_rdata),
        .rresp             (gp_rresp),
        .rlast             (gp_rlast),
        .rid               (gp_rid),
        .rvalid            (gp_rvalid),
        .rready            (gp_rready),
        .pf_wr             (pf_wr),
        .pf_din            (pf_din),
        .pf_free           (pf_free),
        .soft_reset        (soft_reset),
        .src_teensy_en     (src_teensy_en),
        .src_ps_en         (src_ps_en),
        .scanout_en        (scanout_en),
        .fb0_addr          (fb0_addr),
        .fb1_addr          (fb1_addr),
        .clear_color       (clear_color),
        .mmcm_locked       (locked_s[1]),
        .raster_busy       (raster_busy),
        .hpd               (hpd_s[1]),
        .teensy_active     (teensy_active),
        .wait_teensy       (wait_teensy),
        .wait_ps           (wait_ps),
        .swap_pending      (swap_pending),
        .frame_count       (frame_count),
        .front_idx         (front_idx),
        .t_fifo_level      (t_level),
        .list_overflow_cnt (list_overflow_cnt),
        .bad_record_cnt    (bad_record_cnt),
        .t_words           (t_words),
        .render_cycles     (render_cycles),
        .prim_count        (prim_count),
        .vsync_count       (vsync_count),
        .axi_err_cnt       (axi_err_sum),
        .dropped_cnt       (dropped_cnt),
        .ret_addr          (ret_addr),
        .ret_enable        (ret_enable),
        .ret_ack           (ret_ack),
        .ret_full          (ret_full),
        .ret_capturing     (ret_capturing),
        .ret_frame         (ret_frame),
        .last_frame_no     (last_frame_no)
    );

    // =========================================================================================
    // GPU core (collector, lists, raster, strip writer HP1, sprite reader HP2, return capture)
    // =========================================================================================
    core_top #(.LIST_SLOTS(1536)) u_core (
        .clk               (clk),
        .rst               (rst),
        .soft_reset        (soft_reset),
        .src_teensy_en     (src_teensy_en),
        .src_ps_en         (src_ps_en),
        .clear_color       (clear_color),
        .fb0_addr          (fb0_addr),
        .fb1_addr          (fb1_addr),
        .t_empty           (t_empty),
        .t_dout            (t_dout),
        .t_rd              (t_rd),
        .p_empty           (p_empty),
        .p_dout            (p_dout),
        .p_rd              (p_rd),
        .front_idx         (front_idx),
        .swap_req          (swap_req),
        .swap_done         (swap_done),
        .wr_awaddr         (hp1_awaddr),
        .wr_awlen          (hp1_awlen),
        .wr_awvalid        (hp1_awvalid),
        .wr_awready        (hp1_awready),
        .wr_wdata          (hp1_wdata),
        .wr_wlast          (hp1_wlast),
        .wr_wvalid         (hp1_wvalid),
        .wr_wready         (hp1_wready),
        .wr_bresp          (hp1_bresp),
        .wr_bvalid         (hp1_bvalid),
        .wr_bready         (hp1_bready),
        .rd_araddr         (hp2_araddr),
        .rd_arlen          (hp2_arlen),
        .rd_arvalid        (hp2_arvalid),
        .rd_arready        (hp2_arready),
        .rd_rdata          (hp2_rdata),
        .rd_rresp          (hp2_rresp),
        .rd_rlast          (hp2_rlast),
        .rd_rvalid         (hp2_rvalid),
        .rd_rready         (hp2_rready),
        .raster_busy       (raster_busy),
        .wait_teensy       (wait_teensy),
        .wait_ps           (wait_ps),
        .list_overflow_cnt (list_overflow_cnt),
        .bad_record_cnt    (bad_record_cnt),
        .render_cycles     (render_cycles),
        .prim_count        (prim_count),
        .axi_err_cnt       (core_axi_err),
        .dropped_cnt       (dropped_cnt),
        .ret_addr          (ret_addr),
        .ret_enable        (ret_enable),
        .ret_ack           (ret_ack),
        .ret_full          (ret_full),
        .ret_capturing     (ret_capturing),
        .ret_frame         (ret_frame),
        .last_frame_no     (last_frame_no)
    );

    // =========================================================================================
    // Scanout (HP0 reader, async FIFO, video timing, TMDS, OSERDES, OBUFDS)
    // =========================================================================================
    wire [3:0] tmds_p, tmds_n;

    scanout u_scan (
        .clk          (clk),
        .rst          (rst),
        .clk_pix      (clk_pix),
        .clk_ser      (clk_ser),
        .rst_pix      (rst_pix),
        .fb0_addr     (fb0_addr),
        .fb1_addr     (fb1_addr),
        .scanout_en   (scanout_en),
        .swap_req     (swap_req),
        .swap_done    (swap_done),
        .front_idx    (front_idx),
        .frame_count  (frame_count),
        .vsync_count  (vsync_count),
        .swap_pending (swap_pending),
        .rd_araddr    (hp0_araddr),
        .rd_arlen     (hp0_arlen),
        .rd_arvalid   (hp0_arvalid),
        .rd_arready   (hp0_arready),
        .rd_rdata     (hp0_rdata),
        .rd_rresp     (hp0_rresp),
        .rd_rlast     (hp0_rlast),
        .rd_rvalid    (hp0_rvalid),
        .rd_rready    (hp0_rready),
        .axi_err_cnt  (scan_axi_err),
        .tmds_p       (tmds_p),
        .tmds_n       (tmds_n)
    );

    assign hdmi_d0_p  = tmds_p[0];  assign hdmi_d0_n  = tmds_n[0];
    assign hdmi_d1_p  = tmds_p[1];  assign hdmi_d1_n  = tmds_n[1];
    assign hdmi_d2_p  = tmds_p[2];  assign hdmi_d2_n  = tmds_n[2];
    assign hdmi_clk_p = tmds_p[3];  assign hdmi_clk_n = tmds_n[3];
    assign hdmi_out_en = 1'b1;

    // =========================================================================================
    // LEDs
    // =========================================================================================
    // 0.5 s at 148.75 MHz = 74,375,000 cycles. 74,374,999 needs 27 bits (review R1-02: a 26-bit
    // constant silently truncates to 7,266,135 = a ~10 Hz blink).
    localparam [26:0] HB_HALF = 27'd74374999;

    reg [26:0] hb_cnt;
    reg        hb_led;
    always @(posedge clk) begin
        if (rst) begin
            hb_cnt <= 27'd0;
            hb_led <= 1'b0;
        end else if (hb_cnt == HB_HALF) begin
            hb_cnt <= 27'd0;
            hb_led <= ~hb_led;
        end else begin
            hb_cnt <= hb_cnt + 27'd1;
        end
    end

    reg [31:0] fc_prev;
    reg [4:0]  fc_div;
    reg        fr_led;
    always @(posedge clk) begin
        fc_prev <= frame_count;
        if (rst) begin
            fc_div <= 5'd0;
            fr_led <= 1'b0;
        end else if (frame_count != fc_prev) begin
            if (fc_div == 5'd29) begin
                fc_div <= 5'd0;
                fr_led <= ~fr_led;
            end else begin
                fc_div <= fc_div + 5'd1;
            end
        end
    end

    assign led = {fr_led, hb_led};

    // ---- unused inputs from the PS7 ports / unused FIFO outputs (lint) ----
    wire _unused_ok = &{1'b0, t_full_unused, p_full_unused, p_count_unused,
                        hp0_rid, hp0_bid, hp0_bresp, hp0_awready, hp0_wready, hp0_bvalid,
                        hp1_bid, hp1_rid, hp1_arready, hp1_rvalid, hp1_rlast, hp1_rdata, hp1_rresp,
                        hp2_rid, hp2_bid, hp2_bresp, hp2_awready, hp2_wready, hp2_bvalid,
                        gp_arcache, gp_awcache, gp_arlock, gp_awlock, gp_arprot, gp_awprot,
                        gp_arqos, gp_awqos, 1'b0};

endmodule
