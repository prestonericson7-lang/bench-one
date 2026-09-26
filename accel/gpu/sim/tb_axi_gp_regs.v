`timescale 1ns / 1ps
// tb_axi_gp_regs -- rtl/axi_gp_regs.v (+ rtl/sync_fifo.v as the 33 x 512 PS FIFO) against an AXI3
// master BFM and a register/FIFO model.
//
// BFM: independent AW, W, B, AR, R processes working through transaction lists, with random
// VALID gaps and random BREADY/RREADY back-pressure. Write ordering modes per batch:
//   0 = AW and W independent (either may lead), 1 = AW before W, 2 = W before AW (W beats are
//   presented and held before the AW of the same burst is issued).
// Tests:
//   T1 reset: READYs/VALIDs low in reset, control outputs and every register's reset value,
//      unmapped / write-only addresses read 0
//   T2 status inputs (walking ones) and counter inputs read back
//   T3 single writes in all three orderings, T4 WSTRB byte lanes (incl. WSTRB = 0),
//   T5 SOFT_RESET: 1-cycle pulses, bit reads back 0, other CONTROL bits stored;
//      RET_CTRL: RET_ENABLE stored, RET_ACK 1-cycle pulses (SPEC 13.1 registers 0x04C..0x05C)
//   T6 PS FIFO pushes: 0x100 / 0x104 singles, FIXED bursts (every beat pushes), INCR bursts across
//      0x0F8..0x10C, WRAP bursts, narrow (8/16-bit) bursts
//   T7 read bursts INCR / FIXED / WRAP, ID echo, RLAST
//   T8 PS FIFO full: pushes beyond 512 are dropped, PS_FIFO_FREE reads 0, contents intact
//   T9 random: thousands of random bursts (sizes 1/2/4 bytes, FIXED/INCR/WRAP, random strobes,
//      random IDs, random ordering) with concurrent reads of read-only registers, then read-back
// Monitors (every cycle): BVALID/RVALID payload stable until READY, no B before the WLAST of its
//   burst, soft_reset pulse count, every pf_wr word vs model, PS FIFO output vs model.
// Plusargs: +seed=N. Prints "PASS tb_axi_gp_regs" or "FAIL tb_axi_gp_regs".
`include "gpu_defs.vh"

module tb_axi_gp_regs;
    reg clk = 1'b0;
    always #3.361 clk = ~clk;              // 148.75 MHz core clock (6.722 ns)
    reg rst = 1'b1;
    integer seed = 1, seed0 = 1;

    // ------------------------------------------------------------------ DUT signals
    reg  [31:0] awaddr = 0;  reg [3:0] awlen = 0; reg [2:0] awsize = 3'd2; reg [1:0] awburst = 2'b01;
    reg  [11:0] awid = 0;    reg awvalid = 0;     wire awready;
    reg  [31:0] wdata = 0;   reg [3:0] wstrb = 0; reg wlast = 0; reg [11:0] wid = 0; reg wvalid = 0;
    wire        wready;
    wire [1:0]  bresp;       wire [11:0] bid;     wire bvalid;  reg bready = 0;
    reg  [31:0] araddr = 0;  reg [3:0] arlen = 0; reg [2:0] arsize = 3'd2; reg [1:0] arburst = 2'b01;
    reg  [11:0] arid = 0;    reg arvalid = 0;     wire arready;
    wire [31:0] rdata;       wire [1:0] rresp;    wire rlast;   wire [11:0] rid; wire rvalid;
    reg         rready = 0;

    wire        pf_wr;       wire [32:0] pf_din;  wire [9:0] pf_free;
    wire        soft_reset, src_teensy_en, src_ps_en, scanout_en;
    wire [31:0] fb0_addr, fb1_addr;
    wire [15:0] clear_color;

    reg mmcm_locked = 0, raster_busy = 0, hpd = 0, teensy_active = 0, wait_teensy = 0, wait_ps = 0;
    reg swap_pending = 0, front_idx = 0;
    reg [31:0] frame_count = 0, list_overflow_cnt = 0, bad_record_cnt = 0, t_words = 0;
    reg [31:0] render_cycles = 0, prim_count = 0, vsync_count = 0, axi_err_cnt = 0, dropped_cnt = 0;
    reg [10:0] t_fifo_level = 0;
    wire [31:0] ret_addr;
    wire        ret_enable, ret_ack;
    reg         ret_full = 0, ret_capturing = 0;
    reg  [31:0] ret_frame = 0, last_frame_no = 0;

    axi_gp_regs dut (
        .clk(clk), .rst(rst),
        .awaddr(awaddr), .awlen(awlen), .awsize(awsize), .awburst(awburst), .awid(awid),
        .awvalid(awvalid), .awready(awready),
        .wdata(wdata), .wstrb(wstrb), .wlast(wlast), .wid(wid), .wvalid(wvalid), .wready(wready),
        .bresp(bresp), .bid(bid), .bvalid(bvalid), .bready(bready),
        .araddr(araddr), .arlen(arlen), .arsize(arsize), .arburst(arburst), .arid(arid),
        .arvalid(arvalid), .arready(arready),
        .rdata(rdata), .rresp(rresp), .rlast(rlast), .rid(rid), .rvalid(rvalid), .rready(rready),
        .pf_wr(pf_wr), .pf_din(pf_din), .pf_free(pf_free),
        .soft_reset(soft_reset), .src_teensy_en(src_teensy_en), .src_ps_en(src_ps_en),
        .scanout_en(scanout_en), .fb0_addr(fb0_addr), .fb1_addr(fb1_addr), .clear_color(clear_color),
        .mmcm_locked(mmcm_locked), .raster_busy(raster_busy), .hpd(hpd), .teensy_active(teensy_active),
        .wait_teensy(wait_teensy), .wait_ps(wait_ps), .swap_pending(swap_pending),
        .frame_count(frame_count), .front_idx(front_idx), .t_fifo_level(t_fifo_level),
        .list_overflow_cnt(list_overflow_cnt), .bad_record_cnt(bad_record_cnt), .t_words(t_words),
        .render_cycles(render_cycles), .prim_count(prim_count), .vsync_count(vsync_count),
        .axi_err_cnt(axi_err_cnt), .dropped_cnt(dropped_cnt),
        .ret_addr(ret_addr), .ret_enable(ret_enable), .ret_ack(ret_ack), .ret_full(ret_full),
        .ret_capturing(ret_capturing), .ret_frame(ret_frame), .last_frame_no(last_frame_no)
    );

    // PS command FIFO (as in gpu_top) with a controllable consumer
    reg         pop_en = 1'b1;
    reg         p_rd = 1'b0;
    wire [32:0] p_dout;
    wire        p_empty, p_full;
    wire [9:0]  p_count;
    sync_fifo #(.W(33), .AW(9)) pfifo (
        .clk(clk), .rst(rst), .wr(pf_wr), .din(pf_din), .rd(p_rd),
        .dout(p_dout), .empty(p_empty), .full(p_full), .count(p_count), .free(pf_free)
    );

    // ------------------------------------------------------------------ errors / random
    integer errors = 0;
    task err;
        input [8*100-1:0] msg;
        begin
            errors = errors + 1;
            if (errors <= 25) $display("ERROR %t: %0s", $time, msg);
        end
    endtask

    function integer rnd;               // 0 .. n-1
        input integer n;
        begin
            rnd = (n <= 1) ? 0 : (($random(seed) & 32'h7fffffff) % n);
        end
    endfunction

    // ------------------------------------------------------------------ register model
    reg  [3:1]  m_ctrl;
    reg  [31:12] m_fb0, m_fb1;
    reg  [15:0] m_cc;
    reg  [31:12] m_ret;
    reg         m_reten;
    integer     m_soft = 0;                       // expected soft_reset pulses
    integer     m_ack = 0;                        // expected ret_ack pulses
    localparam  MAXQ = 262144;
    reg  [32:0] pq [0:MAXQ-1];                    // expected pf_wr words
    integer     pq_t = 0, pq_h = 0;
    reg  [32:0] fq [0:MAXQ-1];                    // expected PS FIFO contents
    integer     fq_t = 0, fq_h = 0, n_drops = 0;

    task model_reset;
        begin
            m_ctrl = 3'b010;                      // CONTROL reset 0x4 -> SRC_PS
            m_fb0  = 20'h1E000;
            m_fb1  = 20'h1E200;
            m_cc   = 16'h0000;
            m_ret  = 20'h1FE00;                   // RET_ADDR reset 0x1FE00000
            m_reten = 1'b0;
        end
    endtask

    task m_write;                                 // one write beat
        input [11:0] a;
        input [31:0] d;
        input [3:0]  s;
        begin
            case ({a[11:2], 2'b00})
                `R_CONTROL: if (s[0]) begin
                    m_ctrl = d[3:1];
                    if (d[0]) m_soft = m_soft + 1;
                end
                `R_FB0: begin
                    if (s[1]) m_fb0[15:12] = d[15:12];
                    if (s[2]) m_fb0[23:16] = d[23:16];
                    if (s[3]) m_fb0[31:24] = d[31:24];
                end
                `R_FB1: begin
                    if (s[1]) m_fb1[15:12] = d[15:12];
                    if (s[2]) m_fb1[23:16] = d[23:16];
                    if (s[3]) m_fb1[31:24] = d[31:24];
                end
                `R_CLEAR_COLOR: begin
                    if (s[0]) m_cc[7:0]  = d[7:0];
                    if (s[1]) m_cc[15:8] = d[15:8];
                end
                12'h04C: begin                         // RET_ADDR
                    if (s[1]) m_ret[15:12] = d[15:12];
                    if (s[2]) m_ret[23:16] = d[23:16];
                    if (s[3]) m_ret[31:24] = d[31:24];
                end
                12'h050: if (s[0]) begin               // RET_CTRL
                    m_reten = d[0];
                    if (d[1]) m_ack = m_ack + 1;
                end
                `R_PS_FIFO_DATA: if (|s) begin pq[pq_t % MAXQ] = {1'b0, d}; pq_t = pq_t + 1; end
                `R_PS_FIFO_SOR:  if (|s) begin pq[pq_t % MAXQ] = {1'b1, d}; pq_t = pq_t + 1; end
                default: ;
            endcase
        end
    endtask

    function [31:0] exp_read;                     // expected read data for a beat address
        input [11:0] a;
        begin
            case ({a[11:2], 2'b00})
                `R_ID:            exp_read = `GPU_ID_VALUE;
                `R_VERSION:       exp_read = `GPU_VERSION_VALUE;
                `R_CONTROL:       exp_read = {28'd0, m_ctrl, 1'b0};
                `R_STATUS:        exp_read = {25'd0, swap_pending, wait_ps, wait_teensy, teensy_active,
                                              hpd, raster_busy, mmcm_locked};
                `R_FRAME_COUNT:   exp_read = frame_count;
                `R_FB0:           exp_read = {m_fb0, 12'h000};
                `R_FB1:           exp_read = {m_fb1, 12'h000};
                `R_FRONT:         exp_read = {31'd0, front_idx};
                `R_CLEAR_COLOR:   exp_read = {16'd0, m_cc};
                `R_PS_FIFO_FREE:  exp_read = {22'd0, pf_free};
                `R_T_FIFO_LEVEL:  exp_read = {21'd0, t_fifo_level};
                `R_LIST_OVERFLOW: exp_read = list_overflow_cnt;
                `R_BAD_RECORDS:   exp_read = bad_record_cnt;
                `R_T_WORDS:       exp_read = t_words;
                `R_RENDER_CYCLES: exp_read = render_cycles;
                `R_PRIM_COUNT:    exp_read = prim_count;
                `R_VSYNC_COUNT:   exp_read = vsync_count;
                `R_AXI_ERRORS:    exp_read = axi_err_cnt;
                `R_DROPPED:       exp_read = dropped_cnt;
                12'h04C:          exp_read = {m_ret, 12'h000};
                12'h050:          exp_read = {31'd0, m_reten};
                12'h054:          exp_read = {30'd0, ret_capturing, ret_full};
                12'h058:          exp_read = ret_frame;
                12'h05C:          exp_read = last_frame_no;
                default:          exp_read = 32'd0;
            endcase
        end
    endfunction

    // AXI beat address (IHI0022 A3.4.1 formulas, independent of the DUT's iterative version)
    function [11:0] beat_addr;
        input [11:0] start;
        input [2:0]  size;
        input [1:0]  burst;
        input [3:0]  len;
        input integer n;
        integer nb, total, aligned, lower, a;
        begin
            nb      = 1 << size;
            total   = nb * (len + 1);
            aligned = (start / nb) * nb;
            if (burst == 2'b00 || n == 0) a = start;
            else begin
                a = aligned + n * nb;
                if (burst == 2'b10) begin
                    lower = (start / total) * total;
                    if (a >= lower + total) a = a - total;
                end
            end
            beat_addr = a;
        end
    endfunction

    // ------------------------------------------------------------------ transaction lists
    localparam MAXT = 8192;
    reg [11:0] wt_addr [0:MAXT-1];  reg [3:0] wt_len [0:MAXT-1];  reg [2:0] wt_size [0:MAXT-1];
    reg [1:0]  wt_burst[0:MAXT-1];  reg [11:0] wt_id [0:MAXT-1];
    reg [31:0] wb_data [0:MAXT*16-1];
    reg [3:0]  wb_strb [0:MAXT*16-1];
    integer    nwt = 0;
    reg [11:0] rt_addr [0:MAXT-1];  reg [3:0] rt_len [0:MAXT-1];  reg [2:0] rt_size [0:MAXT-1];
    reg [1:0]  rt_burst[0:MAXT-1];  reg [11:0] rt_id [0:MAXT-1];
    integer    nrt = 0;
    reg [31:0] stage_d [0:15];
    reg [3:0]  stage_s [0:15];

    // queue a write burst (data/strobes from stage_d/stage_s) and apply it to the model
    task q_write;
        input [11:0] a;
        input [3:0]  len;
        input [2:0]  size;
        input [1:0]  burst;
        integer n;
        begin
            wt_addr[nwt] = a; wt_len[nwt] = len; wt_size[nwt] = size; wt_burst[nwt] = burst;
            wt_id[nwt] = $random(seed);
            for (n = 0; n <= len; n = n + 1) begin
                wb_data[nwt * 16 + n] = stage_d[n];
                wb_strb[nwt * 16 + n] = stage_s[n];
                m_write(beat_addr(a, size, burst, len, n), stage_d[n], stage_s[n]);
            end
            nwt = nwt + 1;
        end
    endtask

    task q_write1;                                // single 32-bit beat
        input [11:0] a;
        input [31:0] d;
        input [3:0]  s;
        begin
            stage_d[0] = d; stage_s[0] = s;
            q_write(a, 4'd0, 3'd2, 2'b01);
        end
    endtask

    task stage_random;                            // random data, full strobes
        integer n;
        begin
            for (n = 0; n < 16; n = n + 1) begin stage_d[n] = $random(seed); stage_s[n] = 4'hF; end
        end
    endtask

    task q_read;
        input [11:0] a;
        input [3:0]  len;
        input [2:0]  size;
        input [1:0]  burst;
        begin
            rt_addr[nrt] = a; rt_len[nrt] = len; rt_size[nrt] = size; rt_burst[nrt] = burst;
            rt_id[nrt] = $random(seed);
            nrt = nrt + 1;
        end
    endtask

    // ------------------------------------------------------------------ BFM processes
    integer aw_done, w_started, b_done, ar_done, r_done;
    integer wlast_hs = 0, b_hs = 0, r_beats = 0;
    integer gap_aw, gap_w, gap_b, gap_ar, gap_r;  // max random gaps (cycles)

    task aw_proc;
        input integer mode;
        integer t;
        begin
            @(posedge clk);
            for (t = 0; t < nwt; t = t + 1) begin
                if (mode == 2) while (w_started < t) @(posedge clk);
                repeat (rnd(gap_aw + 1) + (mode == 2 ? 1 + rnd(4) : 0)) @(posedge clk);
                awaddr  <= {20'h43C00, wt_addr[t]};
                awlen   <= wt_len[t];  awsize <= wt_size[t]; awburst <= wt_burst[t];
                awid    <= wt_id[t];
                awvalid <= 1'b1;
                @(posedge clk);
                while (!awready) @(posedge clk);
                awvalid <= 1'b0;
                aw_done = t + 1;
            end
        end
    endtask

    task w_proc;
        input integer mode;
        integer t, n;
        begin
            @(posedge clk);
            for (t = 0; t < nwt; t = t + 1) begin
                if (mode == 1) while (aw_done <= t) @(posedge clk);
                for (n = 0; n <= wt_len[t]; n = n + 1) begin
                    repeat (rnd(gap_w + 1)) @(posedge clk);
                    wdata  <= wb_data[t * 16 + n];
                    wstrb  <= wb_strb[t * 16 + n];
                    wlast  <= (n == wt_len[t]);
                    wid    <= wt_id[t];
                    wvalid <= 1'b1;
                    if (n == 0) w_started = t;
                    @(posedge clk);
                    while (!wready) @(posedge clk);
                    wvalid <= 1'b0;
                end
            end
        end
    endtask

    task b_proc;
        integer t;
        begin
            @(posedge clk);
            for (t = 0; t < nwt; t = t + 1) begin
                repeat (rnd(gap_b + 1)) @(posedge clk);
                bready <= 1'b1;
                @(posedge clk);
                while (!bvalid) @(posedge clk);
                if (bid !== wt_id[t])  err("BID does not echo AWID");
                if (bresp !== 2'b00)   err("BRESP != OKAY");
                bready <= 1'b0;
                b_done = t + 1;
            end
        end
    endtask

    task ar_proc;
        integer t;
        begin
            @(posedge clk);
            for (t = 0; t < nrt; t = t + 1) begin
                repeat (rnd(gap_ar + 1)) @(posedge clk);
                araddr  <= {20'h43C00, rt_addr[t]};
                arlen   <= rt_len[t]; arsize <= rt_size[t]; arburst <= rt_burst[t];
                arid    <= rt_id[t];
                arvalid <= 1'b1;
                @(posedge clk);
                while (!arready) @(posedge clk);
                arvalid <= 1'b0;
                ar_done = t + 1;
            end
        end
    endtask

    task r_proc;
        integer t, n;
        reg [31:0] e;
        begin
            @(posedge clk);
            for (t = 0; t < nrt; t = t + 1) begin
                for (n = 0; n <= rt_len[t]; n = n + 1) begin
                    repeat (rnd(gap_r + 1)) @(posedge clk);
                    rready <= 1'b1;
                    @(posedge clk);
                    while (!rvalid) @(posedge clk);
                    e = exp_read(beat_addr(rt_addr[t], rt_size[t], rt_burst[t], rt_len[t], n));
                    if (rdata !== e) begin
                        err("read data mismatch");
                        $display("       txn %0d beat %0d addr %h: got %h expected %h", t, n,
                                 beat_addr(rt_addr[t], rt_size[t], rt_burst[t], rt_len[t], n), rdata, e);
                    end
                    if (rid !== rt_id[t])            err("RID does not echo ARID");
                    if (rresp !== 2'b00)             err("RRESP != OKAY");
                    if (rlast !== (n == rt_len[t]))  err("RLAST wrong");
                    rready <= 1'b0;
                end
                r_done = t + 1;
            end
        end
    endtask

    // run the queued writes and/or reads to completion (concurrently), then clear the lists
    task run;
        input integer mode;
        integer guard;
        begin
            aw_done = 0; w_started = -1; b_done = 0; ar_done = 0; r_done = 0;
            fork
                if (nwt > 0) aw_proc(mode);
                if (nwt > 0) w_proc(mode);
                if (nwt > 0) b_proc;
                if (nrt > 0) ar_proc;
                if (nrt > 0) r_proc;
            join
            nwt = 0; nrt = 0;
            repeat (4) @(posedge clk);
        end
    endtask

    // ------------------------------------------------------------------ monitors
    reg        p_bvalid = 0, p_bready = 0, p_rvalid = 0, p_rready = 0;
    reg [11:0] p_bid, p_rid;
    reg [1:0]  p_bresp, p_rresp;
    reg [31:0] p_rdata;
    reg        p_rlast;
    integer    n_soft_cyc = 0;
    integer    n_ack_cyc = 0;
    integer    mon_cyc = 0;

    always @(negedge clk)
        p_rd <= pop_en && !p_empty;

    always @(posedge clk) begin
        if (rst) begin
            if (mon_cyc >= 2 && (awready !== 1'b0 || arready !== 1'b0 || wready !== 1'b0 ||
                                 bvalid !== 1'b0 || rvalid !== 1'b0)) err("READY/VALID high during reset");
        end
        if (!rst) begin
            // payload stability
            if (p_bvalid && !p_bready && (!bvalid || bid !== p_bid || bresp !== p_bresp))
                err("B channel changed before BREADY");
            if (p_rvalid && !p_rready && (!rvalid || rid !== p_rid || rresp !== p_rresp ||
                                          rdata !== p_rdata || rlast !== p_rlast))
                err("R channel changed before RREADY");
            // no B before the WLAST of its burst (counts from earlier edges)
            if (bvalid && bready) begin
                if (b_hs >= wlast_hs) err("B response before the burst's WLAST was accepted");
                b_hs = b_hs + 1;
            end
            if (wvalid && wready && wlast) wlast_hs = wlast_hs + 1;
            if (rvalid && rready) r_beats = r_beats + 1;
            // soft reset pulses
            if (soft_reset) n_soft_cyc = n_soft_cyc + 1;
            if (ret_ack)    n_ack_cyc  = n_ack_cyc + 1;
            // PS FIFO writes
            if (pf_wr) begin
                if (pq_h >= pq_t) err("unexpected pf_wr");
                else begin
                    if (pf_din !== pq[pq_h % MAXQ]) begin
                        err("pf_din mismatch");
                        $display("       got %h expected %h", pf_din, pq[pq_h % MAXQ]);
                    end
                    pq_h = pq_h + 1;
                end
                if (!p_full) begin fq[fq_t % MAXQ] = pf_din; fq_t = fq_t + 1; end
                else n_drops = n_drops + 1;
            end
            if (p_rd && !p_empty) begin
                if (fq_h >= fq_t) err("unexpected PS FIFO output");
                else begin
                    if (p_dout !== fq[fq_h % MAXQ]) err("PS FIFO output mismatch");
                    fq_h = fq_h + 1;
                end
            end
        end
        mon_cyc = mon_cyc + 1;
        p_bvalid <= bvalid; p_bready <= bready; p_bid <= bid; p_bresp <= bresp;
        p_rvalid <= rvalid; p_rready <= rready; p_rid <= rid; p_rresp <= rresp;
        p_rdata  <= rdata;  p_rlast  <= rlast;
    end

    // ------------------------------------------------------------------ helpers
    task check_outputs;
        input [8*16-1:0] tag;
        begin
            if (src_teensy_en !== m_ctrl[1] || src_ps_en !== m_ctrl[2] || scanout_en !== m_ctrl[3] ||
                fb0_addr !== {m_fb0, 12'h000} || fb1_addr !== {m_fb1, 12'h000} || clear_color !== m_cc ||
                ret_addr !== {m_ret, 12'h000} || ret_enable !== m_reten) begin
                err("control outputs differ from the model");
                $display("       [%0s] ctrl %b%b%b/%b fb0 %h/%h fb1 %h/%h cc %h/%h", tag,
                         scanout_en, src_ps_en, src_teensy_en, m_ctrl, fb0_addr, {m_fb0, 12'h000},
                         fb1_addr, {m_fb1, 12'h000}, clear_color, m_cc);
                $display("       [%0s] ret_addr %h/%h ret_enable %b/%b", tag, ret_addr, {m_ret, 12'h000},
                         ret_enable, m_reten);
            end
            if (n_ack_cyc !== m_ack) begin
                err("ret_ack pulse count");
                $display("       [%0s] ret_ack cycles %0d expected %0d", tag, n_ack_cyc, m_ack);
            end
            if (n_soft_cyc !== m_soft) begin
                err("soft_reset pulse count");
                $display("       [%0s] soft_reset cycles %0d expected %0d", tag, n_soft_cyc, m_soft);
            end
            if (pq_h != pq_t) err("not every expected FIFO push happened");
        end
    endtask

    task read_all_regs;                           // single reads of 0x000..0x05C, 0x100, 0x104 + unmapped
        integer a;
        begin
            for (a = 0; a <= 12'h05C; a = a + 4) q_read(a, 4'd0, 3'd2, 2'b01);
            q_read(12'h100, 4'd0, 3'd2, 2'b01);
            q_read(12'h104, 4'd0, 3'd2, 2'b01);
            q_read(12'h060, 4'd0, 3'd2, 2'b01);
            q_read(12'h0FC, 4'd0, 3'd2, 2'b01);
            q_read(12'h108, 4'd0, 3'd2, 2'b01);
            q_read(12'h800, 4'd0, 3'd2, 2'b01);
            q_read(12'hFFC, 4'd0, 3'd2, 2'b01);
            run(0);
        end
    endtask

    task random_status;
        begin
            {mmcm_locked, raster_busy, hpd, teensy_active, wait_teensy, wait_ps, swap_pending, front_idx} = $random(seed);
            frame_count = $random(seed); list_overflow_cnt = $random(seed); bad_record_cnt = $random(seed);
            t_words = $random(seed); render_cycles = $random(seed); prim_count = $random(seed);
            vsync_count = $random(seed); axi_err_cnt = $random(seed); dropped_cnt = $random(seed);
            t_fifo_level = $random(seed);
            {ret_full, ret_capturing} = $random(seed);
            ret_frame = $random(seed); last_frame_no = $random(seed);
        end
    endtask

    // ------------------------------------------------------------------ tests
    integer i, k, a, len, sz, nb, total, bt, mode, sel;
    integer n_wtx_total = 0, n_rtx_total = 0;
    reg [11:0] base;

    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        seed0 = seed;
        gap_aw = 2; gap_w = 2; gap_b = 2; gap_ar = 2; gap_r = 2;
        model_reset;

        // ---------------- T1 reset
        repeat (8) @(posedge clk);
        @(negedge clk) rst = 1'b0;
        repeat (3) @(posedge clk);
        check_outputs("T1");
        random_status;
        read_all_regs;
        $display("INFO T1 reset values done");

        // ---------------- T2 status walking ones + counters
        for (i = 0; i < 8; i = i + 1) begin
            {mmcm_locked, raster_busy, hpd, teensy_active, wait_teensy, wait_ps, swap_pending, front_idx} = 8'd1 << i;
            q_read(`R_STATUS, 4'd0, 3'd2, 2'b01);
            q_read(`R_FRONT, 4'd0, 3'd2, 2'b01);
            {ret_capturing, ret_full} = (i < 2) ? (2'd1 << i) : 2'd0;
            q_read(12'h054, 4'd0, 3'd2, 2'b01);
            run(0);
        end
        for (i = 0; i < 20; i = i + 1) begin
            random_status;
            q_read(12'h000, 4'd15, 3'd2, 2'b01);     // 0x000..0x03C
            q_read(12'h040, 4'd7, 3'd2, 2'b01);      // 0x040..0x05C
            run(0);
        end

        // ---------------- T3 single writes, all orderings, T4 WSTRB
        for (mode = 0; mode < 3; mode = mode + 1) begin
            q_write1(`R_FB0, 32'h1234_5678, 4'hF);
            q_write1(`R_FB1, 32'hABCD_EF01, 4'hF);
            q_write1(`R_CLEAR_COLOR, 32'hFFFF_F81F, 4'hF);
            q_write1(`R_CONTROL, 32'h0000_000E, 4'hF);
            run(mode);
            check_outputs("T3");
            read_all_regs;
            q_write1(`R_FB0, 32'h5500_0000, 4'b1000);  // byte 3 only
            q_write1(`R_FB0, 32'h0066_0000, 4'b0100);  // byte 2 only
            q_write1(`R_FB0, 32'h0000_7700, 4'b0010);  // byte 1 (bits 15:12 stored)
            q_write1(`R_FB0, 32'h0000_0088, 4'b0001);  // byte 0: nothing stored
            q_write1(`R_FB1, 32'hFFFF_FFFF, 4'b0000);  // WSTRB 0: no change
            q_write1(`R_CLEAR_COLOR, 32'h0000_AB00, 4'b0010);
            q_write1(`R_CLEAR_COLOR, 32'h0000_00CD, 4'b0001);
            q_write1(`R_CLEAR_COLOR, 32'hFFFF_0000, 4'b1100);
            q_write1(`R_CONTROL, 32'hFFFF_FFFF, 4'b1110);  // byte 0 not strobed: no change, no pulse
            q_write1(`R_CONTROL, 32'h0000_0004, 4'b0001);
            q_write1(12'h04C, 32'h1D2C_3FFF, 4'hF);          // RET_ADDR (bits 11:0 dropped)
            q_write1(12'h04C, 32'h00AB_0000, 4'b0100);       // byte 2 only
            q_write1(12'h050, 32'h0000_0001, 4'hF);          // RET_ENABLE = 1
            q_write1(12'h050, 32'h0000_0003, 4'hF);          // RET_ENABLE = 1 + RET_ACK pulse
            q_write1(12'h050, 32'h0000_0002, 4'b0010);       // byte 0 not strobed: nothing
            q_write1(12'h050, 32'h0000_0002, 4'b0001);       // ACK pulse, RET_ENABLE = 0
            q_write1(12'h054, 32'hFFFF_FFFF, 4'hF);          // RET_STATUS read-only
            q_write1(`R_ID, 32'hFFFF_FFFF, 4'hF);          // read-only: ignored
            q_write1(`R_STATUS, 32'hFFFF_FFFF, 4'hF);
            q_write1(12'h800, 32'hFFFF_FFFF, 4'hF);        // unmapped
            run(mode);
            check_outputs("T4");
            read_all_regs;
        end
        $display("INFO T3/T4 single writes + WSTRB done");

        // ---------------- T5 soft reset
        q_write1(`R_CONTROL, 32'h0000_0001, 4'hF);
        run(0);
        check_outputs("T5a");
        q_write1(`R_CONTROL, 32'h0000_000B, 4'hF);         // pulse + SCANOUT_EN + SRC_TEENSY
        stage_d[0] = 32'h3; stage_d[1] = 32'h5; stage_d[2] = 32'h9; stage_d[3] = 32'h0;
        stage_s[0] = 4'hF;  stage_s[1] = 4'hF;  stage_s[2] = 4'hF;  stage_s[3] = 4'hF;
        q_write(`R_CONTROL, 4'd3, 3'd2, 2'b00);             // FIXED burst: 3 pulses (maybe back-to-back)
        run(0);
        check_outputs("T5b");
        read_all_regs;
        $display("INFO T5 soft reset: %0d pulse cycles", n_soft_cyc);

        // ---------------- T6 FIFO pushes
        q_write1(`R_PS_FIFO_SOR, 32'hC0DE_0000, 4'hF);
        q_write1(`R_PS_FIFO_DATA, 32'hC0DE_0001, 4'hF);
        q_write1(`R_PS_FIFO_DATA, 32'hC0DE_0002, 4'b0001);  // any strobe pushes the whole word
        q_write1(`R_PS_FIFO_DATA, 32'hDEAD_DEAD, 4'b0000);  // zero strobe: no push
        stage_random; q_write(`R_PS_FIFO_SOR, 4'd0, 3'd2, 2'b00);
        stage_random; q_write(`R_PS_FIFO_DATA, 4'd15, 3'd2, 2'b00);  // FIXED: 16 pushes
        stage_random; q_write(`R_PS_FIFO_SOR, 4'd7, 3'd2, 2'b00);    // FIXED: 8 SOR pushes
        stage_random; q_write(12'h0F8, 4'd5, 3'd2, 2'b01);           // INCR 0x0F8..0x10C
        stage_random; q_write(12'h100, 4'd3, 3'd2, 2'b10);           // WRAP 0x100..0x10C
        stage_random; q_write(12'h108, 4'd3, 3'd2, 2'b10);           // WRAP 0x108,0x10C,0x100,0x104
        stage_random; q_write(12'h104, 4'd1, 3'd2, 2'b10);           // WRAP 0x104,0x100
        stage_random; q_write(12'h100, 4'd7, 3'd0, 2'b01);           // byte INCR 0x100..0x107
        stage_random; q_write(12'h102, 4'd3, 3'd1, 2'b01);           // half INCR 0x102..0x108
        stage_random; q_write(12'h101, 4'd2, 3'd2, 2'b01);           // unaligned start INCR
        run(0);
        check_outputs("T6");
        pop_en = 1'b1;
        repeat (40) @(posedge clk);
        if (fq_h != fq_t) err("PS FIFO did not deliver every pushed word");
        $display("INFO T6 FIFO pushes: %0d words", pq_t);

        // ---------------- T7 read bursts
        random_status;
        q_read(12'h000, 4'd15, 3'd2, 2'b01);
        q_read(12'h010, 4'd3, 3'd2, 2'b00);          // FIXED
        q_read(12'h018, 4'd3, 3'd2, 2'b10);          // WRAP 0x018,0x01C,0x010,0x014
        q_read(12'h030, 4'd7, 3'd2, 2'b10);          // WRAP 0x030..0x03C,0x020..0x02C
        q_read(12'h040, 4'd15, 3'd2, 2'b01);         // 0x040..0x07C (incl. unmapped)
        q_read(12'hFC0, 4'd15, 3'd2, 2'b01);         // end of the 4 KB window
        q_read(12'h014, 4'd7, 3'd1, 2'b01);          // narrow (16-bit) reads
        q_read(12'h021, 4'd0, 3'd0, 2'b01);          // byte read
        run(0);
        $display("INFO T7 read bursts done");

        // ---------------- T8 PS FIFO full
        pop_en = 1'b0;
        repeat (4) @(posedge clk);
        for (k = 0; k < 33; k = k + 1) begin          // 33 x 16 = 528 pushes > 512
            stage_random; q_write(`R_PS_FIFO_DATA, 4'd15, 3'd2, 2'b00);
        end
        run(0);
        repeat (4) @(posedge clk);
        if (p_count !== 10'd512) err("PS FIFO not full after 528 pushes");
        if (n_drops != 16) begin
            err("expected exactly 16 dropped pushes");
            $display("       drops %0d", n_drops);
        end
        q_read(`R_PS_FIFO_FREE, 4'd0, 3'd2, 2'b01);   // expected {pf_free} = 0
        run(0);
        pop_en = 1'b1;
        repeat (600) @(posedge clk);
        if (fq_h != fq_t) err("PS FIFO contents not fully delivered after the full test");
        q_read(`R_PS_FIFO_FREE, 4'd0, 3'd2, 2'b01);   // 512 again
        run(0);
        check_outputs("T8");
        $display("INFO T8 FIFO full: %0d drops", n_drops);

        // ---------------- T9 random bursts + concurrent read-only reads
        for (k = 0; k < 60; k = k + 1) begin
            gap_aw = rnd(4); gap_w = rnd(4); gap_b = rnd(4); gap_ar = rnd(4); gap_r = rnd(4);
            mode = rnd(3);
            pop_en = (rnd(4) != 0);
            for (i = 0; i < 50; i = i + 1) begin
                sel = rnd(100);
                sz  = (sel < 70) ? 2 : (sel < 85) ? 1 : 0;
                nb  = 1 << sz;
                sel = rnd(100);
                bt  = (sel < 50) ? 1 : (sel < 80) ? 0 : 2;
                if (bt == 2) len = (1 << (1 + rnd(4))) - 1;   // 1, 3, 7, 15
                else         len = rnd(16);
                total = nb * (len + 1);
                case (rnd(12))
                    10: base = 12'h04C;                       // RET_ADDR
                    11: base = 12'h050;                       // RET_CTRL
                    0: base = `R_CONTROL;
                    1: base = `R_FB0;
                    2: base = `R_FB1;
                    3: base = `R_CLEAR_COLOR;
                    4: base = 12'h100;
                    5: base = 12'h104;
                    6: base = 12'h0F0 + rnd(32);
                    7: base = rnd(128);
                    8: base = 12'hFC0 + rnd(64);
                    default: base = rnd(4096);
                endcase
                base = (base / nb) * nb;                      // aligned start
                if (bt == 1 && base + total > 4096) base = 4096 - total;   // no 4 KB crossing
                for (a = 0; a < 16; a = a + 1) begin
                    stage_d[a] = $random(seed);
                    sel = rnd(100);
                    stage_s[a] = (sel < 75) ? 4'hF : (sel < 85) ? 4'h0 : $random(seed);
                    // keep SOFT_RESET pulses rare (bit 0 of CONTROL)
                    if (rnd(4) != 0) stage_d[a][0] = 1'b0;
                end
                q_write(base, len, sz, bt);
                n_wtx_total = n_wtx_total + 1;
            end
            random_status;
            for (i = 0; i < 20; i = i + 1) begin       // concurrent reads of read-only registers
                case (rnd(4))
                    0: q_read(12'h000, rnd(2), 3'd2, 2'b01);          // ID, VERSION
                    1: q_read(`R_STATUS, 4'd1, 3'd2, 2'b01);          // STATUS, FRAME_COUNT
                    2: q_read(`R_T_FIFO_LEVEL, rnd(9), 3'd2, 2'b01);  // 0x028..0x048 counters
                    default: q_read(12'h400 + 4 * rnd(64), rnd(16), 3'd2, 2'b00); // unmapped FIXED
                endcase
                n_rtx_total = n_rtx_total + 1;
            end
            run(mode);
            check_outputs("T9");
            pop_en = 1'b1;
            repeat (600) @(posedge clk);
            if (fq_h != fq_t) err("PS FIFO contents not fully delivered (T9)");
            read_all_regs;                            // RW registers vs model
        end
        $display("INFO T9 random: %0d write bursts, %0d read bursts, %0d pushes, %0d drops, %0d soft-reset pulses, %0d ret_ack pulses",
                 n_wtx_total, n_rtx_total, pq_t, n_drops, m_soft, m_ack);

        // ---------------- reset in the middle: registers return to reset values
        @(negedge clk) rst = 1'b1;
        repeat (3) @(negedge clk);
        rst = 1'b0;
        model_reset;
        fq_h = fq_t;                                  // FIFO flushed by reset
        repeat (3) @(posedge clk);
        check_outputs("reset2");
        read_all_regs;

        $display("INFO B handshakes %0d, WLAST handshakes %0d, R beats %0d", b_hs, wlast_hs, r_beats);
        if (b_hs != wlast_hs) err("number of B responses != number of write bursts");
        $display("INFO simulated time %0t ps", $time);
        if (errors == 0) $display("PASS tb_axi_gp_regs (seed %0d)", seed0);
        else             $display("FAIL tb_axi_gp_regs (%0d errors)", errors);
        $finish;
    end

    initial begin
        #(30_000_000);
        $display("FAIL tb_axi_gp_regs: global timeout (deadlock?)");
        $finish;
    end
endmodule
