// gpu_core.v -- render core with exactly the rtl/INTERFACES.md ports (SPEC sections 5, 7, 9).
// Thin wrapper around core_top with the SPEC 13.1 return capture tied off (ret_enable = 0).
// core_top has the same ports plus ret_addr / ret_enable / ret_ack / ret_full /
// ret_capturing / ret_frame / last_frame_no for the two-way path.
module gpu_core #(parameter LIST_SLOTS = 1536) (
    input  wire        clk, rst, soft_reset,
    input  wire        src_teensy_en, src_ps_en,
    input  wire [15:0] clear_color,
    input  wire [31:0] fb0_addr, fb1_addr,
    // Teensy FIFO read side (FWFT)
    input  wire        t_empty, input wire [32:0] t_dout, output wire t_rd,
    // PS FIFO read side (FWFT)
    input  wire        p_empty, input wire [32:0] p_dout, output wire p_rd,
    // swap handshake with scanout (same clock)
    input  wire        front_idx,
    output wire        swap_req,       // level, raised when back buffer fully written (B responses in)
    input  wire        swap_done,      // 1-cycle pulse from scanout when front flipped
    // AXI3 write master -> S_AXI_HP1 (64-bit)
    output wire [31:0] wr_awaddr, output wire [3:0] wr_awlen, output wire wr_awvalid, input wire wr_awready,
    output wire [63:0] wr_wdata, output wire wr_wlast, output wire wr_wvalid, input wire wr_wready,
    input  wire [1:0]  wr_bresp, input wire wr_bvalid, output wire wr_bready,
    // AXI3 read master -> S_AXI_HP2 (64-bit)
    output wire [31:0] rd_araddr, output wire [3:0] rd_arlen, output wire rd_arvalid, input wire rd_arready,
    input  wire [63:0] rd_rdata, input wire [1:0] rd_rresp, input wire rd_rlast, input wire rd_rvalid,
    output wire rd_rready,
    // status
    output wire        raster_busy, wait_teensy, wait_ps,
    output wire [31:0] list_overflow_cnt, bad_record_cnt, render_cycles, prim_count, axi_err_cnt,
    output wire [31:0] dropped_cnt
);
    /* verilator lint_off UNUSED */
    wire        ret_full, ret_capturing;
    wire [31:0] ret_frame, last_frame_no;
    /* verilator lint_on UNUSED */

    core_top #(.LIST_SLOTS(LIST_SLOTS)) u_top (
        .clk(clk), .rst(rst), .soft_reset(soft_reset),
        .src_teensy_en(src_teensy_en), .src_ps_en(src_ps_en),
        .clear_color(clear_color), .fb0_addr(fb0_addr), .fb1_addr(fb1_addr),
        .t_empty(t_empty), .t_dout(t_dout), .t_rd(t_rd),
        .p_empty(p_empty), .p_dout(p_dout), .p_rd(p_rd),
        .front_idx(front_idx), .swap_req(swap_req), .swap_done(swap_done),
        .wr_awaddr(wr_awaddr), .wr_awlen(wr_awlen), .wr_awvalid(wr_awvalid), .wr_awready(wr_awready),
        .wr_wdata(wr_wdata), .wr_wlast(wr_wlast), .wr_wvalid(wr_wvalid), .wr_wready(wr_wready),
        .wr_bresp(wr_bresp), .wr_bvalid(wr_bvalid), .wr_bready(wr_bready),
        .rd_araddr(rd_araddr), .rd_arlen(rd_arlen), .rd_arvalid(rd_arvalid), .rd_arready(rd_arready),
        .rd_rdata(rd_rdata), .rd_rresp(rd_rresp), .rd_rlast(rd_rlast), .rd_rvalid(rd_rvalid),
        .rd_rready(rd_rready),
        .raster_busy(raster_busy), .wait_teensy(wait_teensy), .wait_ps(wait_ps),
        .list_overflow_cnt(list_overflow_cnt), .bad_record_cnt(bad_record_cnt),
        .render_cycles(render_cycles), .prim_count(prim_count), .axi_err_cnt(axi_err_cnt),
        .dropped_cnt(dropped_cnt),
        .ret_addr(32'd0), .ret_enable(1'b0), .ret_ack(1'b0),
        .ret_full(ret_full), .ret_capturing(ret_capturing), .ret_frame(ret_frame),
        .last_frame_no(last_frame_no)
    );
endmodule
