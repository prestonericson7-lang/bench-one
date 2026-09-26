// core_top.v -- the render core: collector + lists + strip rasteriser + strip writer (HP1) +
// sprite reader (HP2). SPEC sections 5, 7, 9 and 13.1.
//
// Ports = rtl/INTERFACES.md gpu_core ports PLUS the SPEC 13.1 return-capture ports (ret_*,
// last_frame_no); rtl/INTERFACES.md documents both. rtl/gpu_core.v is the gpu_core-port wrapper
// (capture tied off); gpu_top instantiates core_top and wires the ret_* ports to the RET_*
// registers.
//
// Block diagram
//   FIFOs -> core_collector -> list ring (RING slots x 720 bits) + side RAM (strip ranges)
//   core_frame (frame/strip control) -> core_fetch (list walk) -> core_tri / core_sprite
//   core_tri / core_sprite / clear pass -> colour strip bank (s&1, 1280x16) ; core_tri -> Z bank
//   Strip banks hold 128-bit words = 8 pixels (core_tri renders 8 pixels per clock):
//   word = row * 160 + x / 8, pixel x & 7 in bits [16*(x&7) +: 16].
//   core_writer: colour bank -> HP1 bursts, clears the colour bank behind itself
//
// Memories (all inferred block RAM, exact depths, see the per-instance comments):
//   list ring  10 x 2048 x 72 (core_listram)            40 RAMB36
//   side RAM   2048 x 12                                 1 RAMB36 (2 x RAMB18)
//   colour     2 banks x 2560 x 128, byte enables       20 RAMB36 (2 x (2048 + 512) x 128)
//   Z          1 bank  x 2560 x 144, tagged (core_tri)  10 RAMB36 ((2048 + 512) x 144)
//   total 71 RAMB36 (was 137 + 1 RAMB18 with 2 x 1536-record lists and 2 Z banks).
// The two lists (LIST_SLOTS records each, SPEC 7) share RING = 2048 slots; see core_collector
// for the back-pressure when both lists together would exceed the ring.
module core_top #(parameter LIST_SLOTS = 1536) (
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
    output wire        swap_req,
    input  wire        swap_done,
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
    output wire [31:0] dropped_cnt,
    // ---- SPEC 13.1 return capture (rtl/INTERFACES.md, core_top) ----
    input  wire [31:0] ret_addr,       // RET_ADDR register (128-byte aligned; low bits ignored)
    input  wire        ret_enable,     // RET_CTRL bit 0
    input  wire        ret_ack,        // 1-cycle pulse: RET_CTRL write with bit 1 set
    output wire        ret_full,       // RET_STATUS bit 0
    output wire        ret_capturing,  // RET_STATUS bit 1
    output wire [31:0] ret_frame,      // RET_FRAME
    output wire [31:0] last_frame_no   // LAST_FRAME_NO
);
    localparam RING = 2048;         // record slots shared by the two lists (>= LIST_SLOTS)
    localparam SAW  = 11;           // log2(RING)

    // ------------------------------------------------------------------ collector
    wire           lw_en;
    wire [3:0]     lw_col;
    wire [SAW-1:0] lw_addr;
    wire [71:0]    lw_data;
    wire           sw_en;
    wire [SAW-1:0] sw_addr;
    wire [11:0]    sw_data;
    wire           lst_avail, lst_take, lst_free;
    wire [SAW-1:0] lst_start;
    wire [10:0]    lst_cnt;
    wire [31:0]    lst_fno;

    core_collector #(.LIST_SLOTS(LIST_SLOTS), .RING(RING), .SAW(SAW)) u_coll (
        .clk(clk), .rst(rst), .soft_reset(soft_reset),
        .src_teensy_en(src_teensy_en), .src_ps_en(src_ps_en),
        .t_empty(t_empty), .t_dout(t_dout), .t_rd(t_rd),
        .p_empty(p_empty), .p_dout(p_dout), .p_rd(p_rd),
        .lw_en(lw_en), .lw_col(lw_col), .lw_addr(lw_addr), .lw_data(lw_data),
        .sw_en(sw_en), .sw_addr(sw_addr), .sw_data(sw_data),
        .r_avail(lst_avail), .r_start(lst_start), .r_cnt(lst_cnt), .r_fno(lst_fno),
        .r_take(lst_take), .r_free(lst_free),
        .wait_teensy(wait_teensy), .wait_ps(wait_ps),
        .list_overflow_cnt(list_overflow_cnt), .bad_record_cnt(bad_record_cnt),
        .dropped_cnt(dropped_cnt)
    );

    // ------------------------------------------------------------------ list + side RAM
    wire           l_re;
    wire [SAW-1:0] l_raddr;
    wire [719:0]   l_rdata;
    wire           s_re;
    wire [SAW-1:0] s_raddr;
    wire [11:0]    s_rdata;

    core_listram #(.RING(RING), .SAW(SAW)) u_list (
        .clk(clk), .we(lw_en), .wcol(lw_col), .waddr(lw_addr), .wdata(lw_data),
        .re(l_re), .raddr(l_raddr), .rdata(l_rdata)
    );
    core_ram #(.DW(12), .NBE(1), .AW(SAW), .DEPTH(RING), .OREG(1)) u_side (
        .clk(clk), .we(sw_en), .waddr(sw_addr), .wdata(sw_data),
        .re(s_re), .raddr(s_raddr), .rdata(s_rdata)
    );

    // ------------------------------------------------------------------ frame control
    wire        wr_frame_start, wr_frame_done;
    wire [31:0] wr_base, wr_ret_base;
    wire        wr_cap;
    wire [15:0] fcol;
    wire [5:0]  strips_ready, rd_strip;
    wire        bank_clr, bank_clr_idx;
    wire        f_start, f_done, stage_valid, stage_release;
    wire [5:0]  strip;
    wire [10:0] nrec;
    wire [23*32-1:0] stage_words;
    wire        t_start, t_setup_busy, t_idle_all;
    wire        s_start, s_idle;
    wire        cp_we;
    wire [11:0] cp_addr;
    wire        zc_we;
    wire [11:0] zc_addr;

    core_frame u_frame (
        .clk(clk), .rst(rst),
        .lst_avail(lst_avail), .lst_cnt(lst_cnt), .lst_fno(lst_fno),
        .lst_take(lst_take), .lst_free(lst_free),
        .clear_color(clear_color), .fb0_addr(fb0_addr), .fb1_addr(fb1_addr), .front_idx(front_idx),
        .swap_req(swap_req), .swap_done(swap_done),
        .wr_frame_start(wr_frame_start), .wr_base(wr_base), .wr_cap(wr_cap),
        .wr_ret_base(wr_ret_base), .fcol(fcol),
        .strips_ready(strips_ready), .rd_strip(rd_strip), .wr_frame_done(wr_frame_done),
        .bank_clr(bank_clr), .bank_clr_idx(bank_clr_idx),
        .f_start(f_start), .strip(strip), .nrec(nrec), .f_done(f_done),
        .stage_valid(stage_valid), .stage_w0(stage_words[31:0]), .stage_release(stage_release),
        .t_start(t_start), .t_setup_busy(t_setup_busy), .t_idle_all(t_idle_all),
        .s_start(s_start), .s_idle(s_idle),
        .cp_we(cp_we), .cp_addr(cp_addr), .zc_we(zc_we), .zc_addr(zc_addr),
        .ret_addr(ret_addr), .ret_enable(ret_enable), .ret_ack(ret_ack),
        .ret_full(ret_full), .ret_capturing(ret_capturing), .ret_frame(ret_frame),
        .last_frame_no(last_frame_no),
        .raster_busy(raster_busy), .render_cycles(render_cycles), .prim_count(prim_count)
    );

    // the list being rendered is the collector's r_sel: its start slot is stable while rendering
    core_fetch #(.SAW(SAW)) u_fetch (
        .clk(clk), .rst(rst),
        .start(f_start), .strip(strip), .lstart(lst_start), .nrec(nrec), .done(f_done),
        .s_re(s_re), .s_raddr(s_raddr), .s_rdata(s_rdata),
        .l_re(l_re), .l_raddr(l_raddr), .l_rdata(l_rdata),
        .stage_valid(stage_valid), .stage_words(stage_words), .stage_release(stage_release)
    );

    // ------------------------------------------------------------------ triangle unit
    wire        tz_re;
    wire [11:0] tz_raddr;
    wire [143:0] tz_rdata;
    wire [7:0]  t_cwe;
    wire        t_zwe;
    wire [11:0] t_waddr;
    wire [127:0] t_cwdata;
    wire [143:0] t_zwdata;
    /* verilator lint_off UNUSED */
    wire        t_wact;
    /* verilator lint_on UNUSED */

    core_tri u_tri (
        .clk(clk), .rst(rst), .strip(strip),
        .start(t_start), .rec(stage_words), .setup_busy(t_setup_busy), .idle_all(t_idle_all),
        .go_ok(s_idle),
        .z_re(tz_re), .z_raddr(tz_raddr), .z_rdata(tz_rdata),
        .c_we(t_cwe), .z_we(t_zwe), .w_addr(t_waddr), .c_wdata(t_cwdata), .z_wdata(t_zwdata),
        .w_active(t_wact)
    );

    // ------------------------------------------------------------------ sprite unit
    wire [7:0]  s_cwe;
    wire [11:0] s_waddr;
    wire [127:0] s_cwdata;
    wire        s_wact;

    core_sprite u_spr (
        .clk(clk), .rst(rst), .strip(strip),
        .start(s_start), .rec(stage_words), .go_ok(t_idle_all), .idle(s_idle),
        .araddr(rd_araddr), .arlen(rd_arlen), .arvalid(rd_arvalid), .arready(rd_arready),
        .rdata(rd_rdata), .rvalid(rd_rvalid), .rready(rd_rready),
        .c_we(s_cwe), .w_addr(s_waddr), .c_wdata(s_cwdata), .w_active(s_wact)
    );

    // ------------------------------------------------------------------ writer
    wire        wc_re, wc_rbank;
    wire [11:0] wc_raddr;
    wire [127:0] c_rdata0, c_rdata1;
    wire        clr_we, clr_bank;
    wire [11:0] clr_addr;

    core_writer u_wr (
        .clk(clk), .rst(rst),
        .frame_start(wr_frame_start), .base(wr_base), .cap(wr_cap), .ret_base(wr_ret_base),
        .strips_ready(strips_ready),
        .rd_strip(rd_strip), .bank_clr(bank_clr), .bank_clr_idx(bank_clr_idx),
        .frame_done(wr_frame_done),
        .c_re(wc_re), .c_raddr(wc_raddr), .c_rbank(wc_rbank),
        .c_rdata0(c_rdata0), .c_rdata1(c_rdata1),
        .clr_we(clr_we), .clr_bank(clr_bank), .clr_addr(clr_addr),
        .awaddr(wr_awaddr), .awlen(wr_awlen), .awvalid(wr_awvalid), .awready(wr_awready),
        .wdata(wr_wdata), .wlast(wr_wlast), .wvalid(wr_wvalid), .wready(wr_wready),
        .bvalid(wr_bvalid), .bready(wr_bready)
    );

    // ------------------------------------------------------------------ colour banks
    // Raster-side colour writes: tri pipeline / sprite / clear pass are mutually exclusive in
    // time. Per bank the writer's clear has the port while it clears that bank; it never
    // clears the bank the rasteriser is drawing into (checked by tb_core).
    wire [7:0]   r_cwe  = t_cwe | s_cwe | {8{cp_we}};
    wire [11:0]  r_addr = cp_we ? cp_addr : (s_wact ? s_waddr : t_waddr);
    wire [127:0] r_cdat = cp_we ? {8{fcol}} : (s_wact ? s_cwdata : t_cwdata);
    wire        rbank  = strip[0];

    function [15:0] px2be;  // 8 pixel enables -> 16 byte enables
        input [7:0] p;
        integer k;
        begin
            for (k = 0; k < 8; k = k + 1)
                px2be[2*k +: 2] = {2{p[k]}};
        end
    endfunction

    wire        wsel0 = clr_we && !clr_bank;
    wire        wsel1 = clr_we &&  clr_bank;
    wire [15:0]  c_we0 = wsel0 ? 16'hFFFF : (!rbank ? px2be(r_cwe) : 16'h0000);
    wire [15:0]  c_we1 = wsel1 ? 16'hFFFF : ( rbank ? px2be(r_cwe) : 16'h0000);
    wire [11:0]  wa0   = wsel0 ? clr_addr : r_addr;
    wire [11:0]  wa1   = wsel1 ? clr_addr : r_addr;
    wire [127:0] cd0   = wsel0 ? {8{fcol}} : r_cdat;
    wire [127:0] cd1   = wsel1 ? {8{fcol}} : r_cdat;

    // Read enables are gated per bank: a bank's read port is only enabled when its data is
    // used (the writer reads one bank at a time).
    wire        c_re0 = wc_re && !wc_rbank;
    wire        c_re1 = wc_re &&  wc_rbank;

    // 2560 x 128 = (2048 + 512) x 128 each: exactly 10 RAMB36 per bank
    core_ram2 #(.DW(128), .NBE(16), .AW(12), .DEPTH(2560)) u_col0 (
        .clk(clk), .we(c_we0), .waddr(wa0), .wdata(cd0),
        .re(c_re0), .raddr(wc_raddr), .rdata(c_rdata0)
    );
    core_ram2 #(.DW(128), .NBE(16), .AW(12), .DEPTH(2560)) u_col1 (
        .clk(clk), .we(c_we1), .waddr(wa1), .wdata(cd1),
        .re(c_re1), .raddr(wc_raddr), .rdata(c_rdata1)
    );

    // ------------------------------------------------------------------ Z bank (single, tagged)
    // Written by core_tri (whole tagged words) or by core_frame's per-frame tag clear pass
    // (never at the same time: the clear runs only while no strip is being rasterised).
    wire        z_we    = t_zwe || zc_we;
    wire [11:0]  z_waddr = zc_we ? zc_addr : t_waddr;
    wire [143:0] z_wdata = zc_we ? {10'd0, 6'h3F, {128{1'b1}}} : t_zwdata;

    // 2560 x 144 = (2048 + 512) x 144: exactly 10 RAMB36
    core_ram2 #(.DW(144), .NBE(1), .AW(12), .DEPTH(2560)) u_z (
        .clk(clk), .we(z_we), .waddr(z_waddr), .wdata(z_wdata),
        .re(tz_re), .raddr(tz_raddr), .rdata(tz_rdata)
    );

    // ------------------------------------------------------------------ AXI error counter
    reg [31:0] axi_err;
    always @(posedge clk) begin
        if (rst)
            axi_err <= 32'd0;
        else
            axi_err <= axi_err + ((wr_bvalid && wr_bready && (wr_bresp != 2'b00)) ? 32'd1 : 32'd0)
                               + ((rd_rvalid && rd_rready && (rd_rresp != 2'b00)) ? 32'd1 : 32'd0);
    end
    assign axi_err_cnt = axi_err;

    // rd_rlast is not needed: ID-0 read data returns in order and the sprite unit counts beats.
    /* verilator lint_off UNUSED */
    wire unused_ok = rd_rlast;
    /* verilator lint_on UNUSED */
endmodule
