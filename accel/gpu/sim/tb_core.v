// tb_core.v -- testbench for gpu_core (Icarus Verilog 11).
//
// Run from a work directory that contains the scene files (sim/run_core.sh does this):
//   rec.hex     32-bit words, records of 24 words (sor = word index % 24 == 0)
//   ddr.hex     optional, $readmemh image of a 64-bit x 1M-word memory at 0x1E000000
//   ps.hex / teensy.hex   optional "stream" files (override rec.hex), 12 hex digits per line:
//       [47:44] op  [32] sor  [31:0] data
//       op 0 PUSH {sor,data} into this stream's FIFO     op 1 WAIT until swaps >= data
//       op 2 SOFT_RESET pulse (flushes both FIFOs)         op 3 CONTROL = data (bit1 T, bit2 PS)
//       op 4 CLEAR_COLOR = data[15:0]                     op 5 WAIT data cycles
//       op 6 WAIT until this FIFO is empty                op 7 RET_ENABLE = data[0]
//       op 8 RET_ACK pulse                                 op 15 end of stream
// Plusargs: +nwords=N (words in rec.hex) +teensy (rec.hex via the Teensy FIFO)
//   +retcap (RET_ENABLE = 1 from reset: the first frame is captured, later ones are not
//   because nobody acknowledges) +swapsrc (stream scenes: ps.hex drives the Teensy FIFO and teensy.hex the PS FIFO, and
//   CONTROL bits 1/2 are swapped -- equivalent for single-source scenes, used to run them
//   through the other FIFO path)
//   +clear=XXXX +nframes=N +seed=N +stall=P (0..90, % of cycles a ready/valid is withheld)
//   +maxcycles=N +ret_addr=XXXXXXXX (return-capture buffer, default 1E600000 = inside the model)
//   default DUT: core_top (gpu_core ports + SPEC 13.1 ret_* ports); compile with
//   -DNO_RET_PORTS to test the INTERFACES.md wrapper gpu_core instead (capture tied off).
// Outputs: actual_<n>.hex for frame n (230400 lines of 16 hex digits), log lines
//   "FRAME n render_cycles=.. prim_count=.. overflow=.. bad=.. dropped=.. axi_err=.."
//   "RETCAP n fno=X" when frame n was captured (ret_<n>.hex = return buffer contents)
//   "TB_RESULT errors=N frames=N"
`timescale 1ns/1ps
`ifdef NO_RET_PORTS
`define CORE dut.u_top
`else
`define CORE dut
`endif
module tb_core;
`ifndef LIST_SLOTS
`define LIST_SLOTS 1536
`endif
    localparam [31:0] DDR_BASE = 32'h1E000000;
    localparam integer DDR_WORDS = 1048576;
    localparam integer FRAME_WORDS = 230400;
    localparam integer MAXS = 262144;      // stream entries

    // ---------------------------------------------------------------- clock / reset
    reg clk = 1'b0;
    always #3.3615 clk = ~clk;             // 148.75 MHz (cycle counts do not depend on it)
    reg rst = 1'b1;
    reg soft_reset = 1'b0;
    reg src_teensy_en = 1'b0;
    reg src_ps_en = 1'b0;
    reg [15:0] clear_color = 16'h0000;
    reg [31:0] fb0_addr = 32'h1E000000;
    reg [31:0] fb1_addr = 32'h1E200000;
    reg front_idx = 1'b0;
    reg swap_done = 1'b0;
    reg [31:0] ret_addr = 32'h1E600000;
    reg ret_enable = 1'b0;
    reg ret_ack = 1'b0;
    wire ret_full, ret_capturing;
    wire [31:0] ret_frame, last_frame_no;

    integer seed = 1;
    integer stall = 30;
    integer nframes = 1;
    integer maxcycles = 40000000;
    integer errors = 0;
    integer cyc = 0;
    integer swaps = 0;

    function rnd_hit;              // 1 with probability pct %
        input integer pct;
        integer r;
        begin
            r = $random(seed);
            if (r < 0) r = -r;
            rnd_hit = ((r % 100) < pct);
        end
    endfunction
    function integer rnd_range;    // lo..hi
        input integer lo, hi;
        integer r;
        begin
            r = $random(seed);
            if (r < 0) r = -r;
            rnd_range = lo + (r % (hi - lo + 1));
        end
    endfunction

    // ---------------------------------------------------------------- DUT
    wire        t_empty, p_empty, t_rd, p_rd;
    wire [32:0] t_dout, p_dout;
    wire        swap_req;
    wire [31:0] wr_awaddr; wire [3:0] wr_awlen; wire wr_awvalid; reg wr_awready = 1'b0;
    wire [63:0] wr_wdata; wire wr_wlast, wr_wvalid; reg wr_wready = 1'b0;
    reg  [1:0]  wr_bresp = 2'b00; reg wr_bvalid = 1'b0; wire wr_bready;
    wire [31:0] rd_araddr; wire [3:0] rd_arlen; wire rd_arvalid; reg rd_arready = 1'b0;
    reg  [63:0] rd_rdata = 64'd0; reg [1:0] rd_rresp = 2'b00; reg rd_rlast = 1'b0; reg rd_rvalid = 1'b0;
    wire        rd_rready;
    wire        raster_busy, wait_teensy, wait_ps;
    wire [31:0] list_overflow_cnt, bad_record_cnt, render_cycles, prim_count, axi_err_cnt, dropped_cnt;

`ifdef NO_RET_PORTS
    gpu_core #(.LIST_SLOTS(`LIST_SLOTS)) dut (
`else
    core_top #(.LIST_SLOTS(`LIST_SLOTS)) dut (
`endif
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
        .dropped_cnt(dropped_cnt)
`ifndef NO_RET_PORTS
        , .ret_addr(ret_addr), .ret_enable(ret_enable), .ret_ack(ret_ack),
        .ret_full(ret_full), .ret_capturing(ret_capturing), .ret_frame(ret_frame),
        .last_frame_no(last_frame_no)
`endif
    );
`ifdef NO_RET_PORTS
    assign ret_full = dut.u_top.ret_full;
    assign ret_capturing = dut.u_top.ret_capturing;
    assign ret_frame = dut.u_top.ret_frame;
    assign last_frame_no = dut.u_top.last_frame_no;
`endif

    // ---------------------------------------------------------------- DDR model
    reg [63:0] ddr [0:DDR_WORDS-1];

    function integer ddr_index;
        input [31:0] a;
        begin
            ddr_index = (a - DDR_BASE) >> 3;
        end
    endfunction
    function in_ddr;
        input [31:0] a;
        begin
            in_ddr = (a >= DDR_BASE) && (a < DDR_BASE + DDR_WORDS * 8);
        end
    endfunction

    // ---------------------------------------------------------------- streams
    reg [47:0] strm_t [0:MAXS-1];
    reg [47:0] strm_p [0:MAXS-1];
    reg [31:0] recw   [0:MAXS-1];
    integer nwords = 0;
    reg use_teensy = 1'b0;
    reg swapsrc = 1'b0;

    // FIFO models (FWFT)
    reg [32:0] tq [0:1023];
    reg [32:0] pq [0:511];
    integer t_wp = 0, t_rp = 0, t_cnt = 0;
    integer p_wp = 0, p_rp = 0, p_cnt = 0;
    assign t_empty = (t_cnt == 0);
    assign p_empty = (p_cnt == 0);
    assign t_dout  = tq[t_rp];
    assign p_dout  = pq[p_rp];

    integer si_t = 0, si_p = 0;          // stream indices
    integer wait_t = 0, wait_p = 0;      // WAIT counters
    reg     done_t = 1'b0, done_p = 1'b0;
    reg     started = 1'b0;

    // one stream engine step; returns push request. Global ops act directly.
    reg        push_t, push_p;
    reg [32:0] pdata_t, pdata_p;
    reg        req_srst;
    reg        req_ack;

    task stream_step;
        input        which;           // 0 = teensy, 1 = ps
        output       push;
        output [32:0] pdata;
        reg [47:0]   e;
        integer      idx, cnt, cap, wt;
        reg          dn;
        begin
            push = 1'b0;
            pdata = 33'd0;
            idx = which ? si_p : si_t;
            cnt = which ? p_cnt : t_cnt;
            cap = which ? 512 : 1024;
            wt  = which ? wait_p : wait_t;
            dn  = which ? done_p : done_t;
            if (!dn) begin
                e = which ? strm_p[idx] : strm_t[idx];
                case (e[47:44])
                    4'd0: begin
                        if (cnt < cap && !rnd_hit(stall)) begin
                            push = 1'b1;
                            pdata = e[32:0];
                            idx = idx + 1;
                        end
                    end
                    4'd1: if (swaps >= e[31:0]) idx = idx + 1;
                    4'd2: begin req_srst = 1'b1; idx = idx + 1; end
                    4'd3: begin
                        src_teensy_en <= swapsrc ? e[2] : e[1];
                        src_ps_en <= swapsrc ? e[1] : e[2];
                        idx = idx + 1;
                    end
                    4'd4: begin clear_color <= e[15:0]; idx = idx + 1; end
                    4'd5: begin
                        if (wt == 0) wt = e[31:0];
                        else begin
                            wt = wt - 1;
                            if (wt == 0) idx = idx + 1;
                        end
                        if (e[31:0] == 0) idx = idx + 1;
                    end
                    4'd6: if (cnt == 0) idx = idx + 1;
                    4'd7: begin ret_enable <= e[0]; idx = idx + 1; end
                    4'd8: begin req_ack = 1'b1; idx = idx + 1; end
                    4'd15: dn = 1'b1;
                    default: begin
                        $display("TB ERROR: bad stream op %h", e);
                        errors = errors + 1;
                        dn = 1'b1;
                    end
                endcase
            end
            if (which) begin si_p = idx; wait_p = wt; done_p = dn; end
            else       begin si_t = idx; wait_t = wt; done_t = dn; end
        end
    endtask

    // All state the DUT samples is updated with nonblocking assignments (no race with the DUT).
    integer n_trp, n_twp, n_tcnt, n_prp, n_pwp, n_pcnt;
    always @(posedge clk) begin
        if (started && !rst) begin
            if (t_rd && t_cnt == 0) begin $display("TB ERROR: t_rd on empty FIFO"); errors = errors + 1; end
            if (p_rd && p_cnt == 0) begin $display("TB ERROR: p_rd on empty FIFO"); errors = errors + 1; end
            if (soft_reset) begin
                $display("SOFTRESET cycle=%0d raster_busy=%b ring_used=%0d ring_full=%b t_cnt=%0d p_cnt=%0d",
                         cyc, raster_busy, `CORE.u_coll.used, `CORE.u_coll.no_space_r, t_cnt, p_cnt);
                // gpu_top resets both FIFOs with soft_reset
                t_wp <= 0; t_rp <= 0; t_cnt <= 0;
                p_wp <= 0; p_rp <= 0; p_cnt <= 0;
                soft_reset <= 1'b0;
            end else begin
                req_srst = 1'b0;
                req_ack = 1'b0;
                stream_step(1'b0, push_t, pdata_t);
                stream_step(1'b1, push_p, pdata_p);
                n_trp = t_rp; n_twp = t_wp; n_tcnt = t_cnt;
                n_prp = p_rp; n_pwp = p_wp; n_pcnt = p_cnt;
                if (t_rd && t_cnt > 0) begin n_trp = (t_rp + 1) % 1024; n_tcnt = n_tcnt - 1; end
                if (p_rd && p_cnt > 0) begin n_prp = (p_rp + 1) % 512;  n_pcnt = n_pcnt - 1; end
                if (push_t) begin tq[t_wp] <= pdata_t; n_twp = (t_wp + 1) % 1024; n_tcnt = n_tcnt + 1; end
                if (push_p) begin pq[p_wp] <= pdata_p; n_pwp = (p_wp + 1) % 512;  n_pcnt = n_pcnt + 1; end
                t_rp <= n_trp; t_wp <= n_twp; t_cnt <= n_tcnt;
                p_rp <= n_prp; p_wp <= n_pwp; p_cnt <= n_pcnt;
                if (req_srst) soft_reset <= 1'b1;
                ret_ack <= req_ack;
            end
        end
    end

    // ---------------------------------------------------------------- HP1 write slave
    localparam AQ = 16;
    reg [31:0] aq_addr [0:AQ-1];
    integer aq_wp = 0, aq_rp = 0, aq_cnt = 0;
    integer wbeat = 0;
    integer bq_time [0:255];
    integer bq_wp = 0, bq_rp = 0, bq_cnt = 0;
    integer aw_max = 8;
    reg [31:0] p_awaddr; reg [3:0] p_awlen; reg p_aw_stall = 1'b0;
    reg [63:0] p_wdata; reg p_wlast; reg p_w_stall = 1'b0;
    reg [31:0] p_araddr; reg [3:0] p_arlen; reg p_ar_stall = 1'b0;
    reg [31:0] back_base;
    reg [7:0]  bcov [0:14399];
    reg [7:0]  rcov [0:14399];
    wire [31:0] ret_base_l = {`CORE.wr_ret_base[31:7], 7'd0};
    integer frame_bursts = 0;
    integer k;

    always @(posedge clk) begin
        if (!rst) begin
            // ---- stability checks (payload stable while VALID && !READY)
            if (p_aw_stall && (!wr_awvalid || wr_awaddr !== p_awaddr || wr_awlen !== p_awlen)) begin
                $display("TB ERROR: AW changed while stalled at cycle %0d", cyc); errors = errors + 1;
            end
            if (p_w_stall && (!wr_wvalid || wr_wdata !== p_wdata || wr_wlast !== p_wlast)) begin
                $display("TB ERROR: W changed while stalled at cycle %0d", cyc); errors = errors + 1;
            end
            if (p_ar_stall && (!rd_arvalid || rd_araddr !== p_araddr || rd_arlen !== p_arlen)) begin
                $display("TB ERROR: AR changed while stalled at cycle %0d", cyc); errors = errors + 1;
            end
            p_aw_stall <= wr_awvalid && !wr_awready;
            p_awaddr <= wr_awaddr; p_awlen <= wr_awlen;
            p_w_stall <= wr_wvalid && !wr_wready;
            p_wdata <= wr_wdata; p_wlast <= wr_wlast;
            p_ar_stall <= rd_arvalid && !rd_arready;
            p_araddr <= rd_araddr; p_arlen <= rd_arlen;

            // ---- AW
            if (wr_awvalid && wr_awready) begin
                back_base = front_idx ? fb0_addr : fb1_addr;
                if (wr_awlen != 4'd15 || wr_awaddr[6:0] != 7'd0) begin
                    $display("TB ERROR: AW not a 128-byte aligned 16-beat burst: %h len %0d", wr_awaddr, wr_awlen);
                    errors = errors + 1;
                end
                if (((wr_awaddr & 32'hFFF) + (wr_awlen + 1) * 8) > 4096) begin
                    $display("TB ERROR: AW crosses 4 KB: %h len %0d", wr_awaddr, wr_awlen); errors = errors + 1;
                end
                if (wr_awaddr >= back_base && wr_awaddr < back_base + 32'd1843200) begin
                    bcov[(wr_awaddr - back_base) >> 7] = bcov[(wr_awaddr - back_base) >> 7] + 1;
                end else if (ret_capturing && wr_awaddr >= ret_base_l && wr_awaddr < ret_base_l + 32'd1843200) begin
                    rcov[(wr_awaddr - ret_base_l) >> 7] = rcov[(wr_awaddr - ret_base_l) >> 7] + 1;
                end else begin
                    $display("TB ERROR: AW outside back buffer (and return buffer): %h (back %h)", wr_awaddr, back_base);
                    errors = errors + 1;
                end
                aq_addr[aq_wp] = wr_awaddr;
                aq_wp = (aq_wp + 1) % AQ;
                aq_cnt = aq_cnt + 1;
            end
            // ---- W
            if (wr_wvalid && wr_wready) begin
                if (aq_cnt == 0) begin
                    $display("TB ERROR: W beat before its AW at cycle %0d", cyc); errors = errors + 1;
                end else begin
                    if (in_ddr(aq_addr[aq_rp] + wbeat * 8))
                        ddr[ddr_index(aq_addr[aq_rp] + wbeat * 8)] = wr_wdata;
                    if (wr_wlast !== (wbeat == 15)) begin
                        $display("TB ERROR: WLAST wrong at beat %0d", wbeat); errors = errors + 1;
                    end
                    wbeat = wbeat + 1;
                    if (wbeat == 16) begin
                        wbeat = 0;
                        aq_rp = (aq_rp + 1) % AQ;
                        aq_cnt = aq_cnt - 1;
                        bq_time[bq_wp] = cyc + (stall == 0 ? 2 : rnd_range(2, 40));
                        bq_wp = (bq_wp + 1) % 256;
                        bq_cnt = bq_cnt + 1;
                        frame_bursts = frame_bursts + 1;
                    end
                end
            end
            // ---- B
            if (wr_bvalid && wr_bready) begin
                bq_rp = (bq_rp + 1) % 256;
                bq_cnt = bq_cnt - 1;
            end
            if (!wr_bready && wr_bvalid) begin
                // allowed, just hold
            end
            if (wr_bvalid && !wr_bready) wr_bvalid <= 1'b1;
            else wr_bvalid <= (bq_cnt > 0) && (bq_time[bq_rp] <= cyc) && !rnd_hit(stall);
            wr_awready <= (aq_cnt < aw_max) && !rnd_hit(stall);
            wr_wready  <= !rnd_hit(stall);
        end
    end

    // ---------------------------------------------------------------- HP2 read slave
    localparam RQ = 16;
    reg [31:0] rq_addr [0:RQ-1];
    reg [3:0]  rq_len  [0:RQ-1];
    integer    rq_time [0:RQ-1];
    integer rq_wp = 0, rq_rp = 0, rq_cnt = 0;
    integer rbeat = 0;
    integer ar_max = 8;
    integer rd_beats_total = 0;
    integer ring_stall = 0;               // cycles the collector held a non-empty FIFO: ring full
    always @(posedge clk)
        if (!rst && `CORE.u_coll.stall && ((`CORE.u_coll.t_cur && !t_empty) || (`CORE.u_coll.p_cur && !p_empty)))
            ring_stall = ring_stall + 1;
    reg [31:0] ra;

    always @(posedge clk) begin
        if (!rst) begin
            if (rd_arvalid && rd_arready) begin
                if (rd_araddr[2:0] != 3'd0) begin
                    $display("TB ERROR: AR unaligned %h", rd_araddr); errors = errors + 1;
                end
                if (((rd_araddr & 32'hFFF) + (rd_arlen + 1) * 8) > 4096) begin
                    $display("TB ERROR: AR crosses 4 KB: %h len %0d", rd_araddr, rd_arlen); errors = errors + 1;
                end
                if (!in_ddr(rd_araddr) || !in_ddr(rd_araddr + rd_arlen * 8)) begin
                    $display("TB ERROR: AR outside DDR model: %h", rd_araddr); errors = errors + 1;
                end
                rq_addr[rq_wp] = rd_araddr;
                rq_len[rq_wp]  = rd_arlen;
                rq_time[rq_wp] = cyc + (stall == 0 ? 8 : rnd_range(8, 60));
                rq_wp = (rq_wp + 1) % RQ;
                rq_cnt = rq_cnt + 1;
            end
            if (rd_rvalid && rd_rready) begin
                rd_beats_total = rd_beats_total + 1;
                if (rbeat == rq_len[rq_rp]) begin
                    rbeat = 0;
                    rq_rp = (rq_rp + 1) % RQ;
                    rq_cnt = rq_cnt - 1;
                end else begin
                    rbeat = rbeat + 1;
                end
            end
            // present next beat
            if (rd_rvalid && !rd_rready) begin
                // hold
            end else if (rq_cnt > 0 && rq_time[rq_rp] <= cyc && !rnd_hit(stall)) begin
                ra = rq_addr[rq_rp] + rbeat * 8;
                rd_rvalid <= 1'b1;
                rd_rdata  <= in_ddr(ra) ? ddr[ddr_index(ra)] : 64'hDEADBEEFDEADBEEF;
                rd_rlast  <= (rbeat == rq_len[rq_rp]);
            end else begin
                rd_rvalid <= 1'b0;
            end
            rd_arready <= (rq_cnt < ar_max) && !rnd_hit(stall);
        end
    end

    // ---------------------------------------------------------------- internal assertions
    always @(posedge clk) begin
        if (!rst) begin
            if (`CORE.wsel0 && !`CORE.rbank && (`CORE.r_cwe != 4'b0000)) begin
                $display("TB ERROR: bank 0 write conflict (writer clear vs raster) at cycle %0d", cyc);
                errors = errors + 1;
            end
            if (`CORE.wsel1 && `CORE.rbank && (`CORE.r_cwe != 4'b0000)) begin
                $display("TB ERROR: bank 1 write conflict (writer clear vs raster) at cycle %0d", cyc);
                errors = errors + 1;
            end
            if (`CORE.zc_we && (`CORE.t_zwe || `CORE.tz_re || `CORE.u_tri.s0_v || `CORE.u_tri.s1_v ||
                                `CORE.u_tri.s2_v || `CORE.u_tri.s3_v)) begin
                $display("TB ERROR: Z tag clear overlaps triangle Z traffic at cycle %0d", cyc);
                errors = errors + 1;
            end
            if (((`CORE.t_cwe != 4'b0000) || `CORE.t_wact) && ((`CORE.s_cwe != 4'b0000) || `CORE.s_wact)) begin
                $display("TB ERROR: tri and sprite write in the same cycle at %0d", cyc); errors = errors + 1;
            end
            if (`CORE.cp_we && (`CORE.t_wact || `CORE.s_wact)) begin
                $display("TB ERROR: clear pass overlaps primitive writes at %0d", cyc); errors = errors + 1;
            end
        end
    end

    // ---------------------------------------------------------------- swap emulation + dump
    reg     swap_pend = 1'b0;
    integer swap_at = 0;
    integer fd, i;
    reg [8*32-1:0] fname;
    reg [31:0] bb;
    reg        ret_full_d = 1'b0;
    reg        cap_done;
    always @(posedge clk) ret_full_d <= ret_full;

    always @(posedge clk) begin
        cyc = cyc + 1;
        swap_done <= 1'b0;
        if (!rst && swap_req && !swap_pend && !swap_done) begin
            swap_pend = 1'b1;
            swap_at = cyc + (stall == 0 ? 5 : rnd_range(5, 300));
            bb = front_idx ? fb0_addr : fb1_addr;
            $display("FRAME %0d render_cycles=%0d prim_count=%0d overflow=%0d bad=%0d dropped=%0d axi_err=%0d fno=%0d back=%h cycle=%0d",
                     swaps, render_cycles, prim_count, list_overflow_cnt, bad_record_cnt, dropped_cnt,
                     axi_err_cnt, `CORE.u_frame.fno, bb, cyc);
            cap_done = ret_full && !ret_full_d;
            if (frame_bursts != (cap_done ? 28800 : 14400)) begin
                $display("TB ERROR: frame had %0d write bursts (expected %0d)", frame_bursts,
                         cap_done ? 28800 : 14400);
                errors = errors + 1;
            end
            for (i = 0; i < 14400; i = i + 1) begin
                if (rcov[i] != (cap_done ? 8'd1 : 8'd0)) begin
                    if (errors < 20)
                        $display("TB ERROR: return burst %0d written %0d times (capture %0d)", i, rcov[i], cap_done);
                    errors = errors + 1;
                end
                rcov[i] = 8'd0;
            end
            if (cap_done) begin
                $display("RETCAP %0d fno=%0d ret_frame=%0d", swaps, `CORE.u_frame.fno, ret_frame);
                if (ret_frame !== `CORE.u_frame.fno) begin
                    $display("TB ERROR: RET_FRAME %0d != frame_no %0d", ret_frame, `CORE.u_frame.fno);
                    errors = errors + 1;
                end
                $sformat(fname, "ret_%0d.hex", swaps);
                fd = $fopen(fname, "w");
                for (i = 0; i < FRAME_WORDS; i = i + 1)
                    $fwrite(fd, "%016h\n", ddr[ddr_index(ret_base_l) + i]);
                $fclose(fd);
            end
            for (i = 0; i < 14400; i = i + 1) begin
                if (bcov[i] != 8'd1) begin
                    if (errors < 20)
                        $display("TB ERROR: burst %0d of frame written %0d times", i, bcov[i]);
                    errors = errors + 1;
                end
                bcov[i] = 8'd0;
            end
            frame_bursts = 0;
            if (bq_cnt != 0) begin
                $display("TB ERROR: swap_req with %0d B responses outstanding", bq_cnt); errors = errors + 1;
            end
            $sformat(fname, "actual_%0d.hex", swaps);
            fd = $fopen(fname, "w");
            for (i = 0; i < FRAME_WORDS; i = i + 1)
                $fwrite(fd, "%016h\n", ddr[ddr_index(bb) + i]);
            $fclose(fd);
        end
        if (swap_pend && cyc >= swap_at) begin
            if (!swap_req) begin
                $display("TB ERROR: swap_req dropped before swap_done"); errors = errors + 1;
            end
            swap_pend = 1'b0;
            swap_done <= 1'b1;
            front_idx <= ~front_idx;
            swaps = swaps + 1;
        end
        if (!rst && swap_done && !swap_req) begin
            // fine
        end
    end

    // ---------------------------------------------------------------- main
    reg [15:0] clr_arg;
    integer fdt, ctl, j;
    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        if ($value$plusargs("stall=%d", stall)) ;
        if ($value$plusargs("nframes=%d", nframes)) ;
        if ($value$plusargs("maxcycles=%d", maxcycles)) ;
        if ($value$plusargs("nwords=%d", nwords)) ;
        if ($value$plusargs("clear=%h", clr_arg)) clear_color = clr_arg;
        if ($value$plusargs("ret_addr=%h", ret_addr)) ;
        use_teensy = $test$plusargs("teensy");
        swapsrc = $test$plusargs("swapsrc");
        if ($test$plusargs("retcap")) ret_enable = 1'b1;

        for (i = 0; i < DDR_WORDS; i = i + 1) ddr[i] = 64'd0;
        for (i = 0; i < 14400; i = i + 1) begin bcov[i] = 8'd0; rcov[i] = 8'd0; end
        fdt = $fopen("ddr.hex", "r");
        if (fdt != 0) begin $fclose(fdt); $readmemh("ddr.hex", ddr); end

        for (i = 0; i < MAXS; i = i + 1) begin strm_t[i] = {4'hF, 44'd0}; strm_p[i] = {4'hF, 44'd0}; end
        ctl = 0;
        fdt = $fopen("ps.hex", "r");
        if (fdt != 0) begin
            $fclose(fdt); ctl = -1;
            if (swapsrc) $readmemh("ps.hex", strm_t); else $readmemh("ps.hex", strm_p);
        end
        fdt = $fopen("teensy.hex", "r");
        if (fdt != 0) begin
            $fclose(fdt); ctl = -1;
            if (swapsrc) $readmemh("teensy.hex", strm_p); else $readmemh("teensy.hex", strm_t);
        end
        if (ctl == 0) begin
            // plain rec.hex -> one stream
            $readmemh("rec.hex", recw, 0, nwords - 1);
            for (j = 0; j < nwords; j = j + 1) begin
                if (use_teensy) strm_t[j] = {4'h0, 11'd0, (j % 24 == 0) ? 1'b1 : 1'b0, recw[j]};
                else            strm_p[j] = {4'h0, 11'd0, (j % 24 == 0) ? 1'b1 : 1'b0, recw[j]};
            end
            if (use_teensy) begin src_teensy_en = 1'b1; src_ps_en = 1'b0; end
            else            begin src_teensy_en = 1'b0; src_ps_en = 1'b1; end
        end
        $display("TB: seed=%0d stall=%0d nframes=%0d nwords=%0d path=%s streams=%0d clear=%h",
                 seed, stall, nframes, nwords, use_teensy ? "teensy" : "ps", (ctl != 0), clear_color);

        repeat (20) @(posedge clk);
        rst <= 1'b0;
        repeat (5) @(posedge clk);
        started <= 1'b1;

        while (swaps < nframes && cyc < maxcycles) @(posedge clk);
        if (swaps < nframes) begin
            $display("TB ERROR: timeout after %0d cycles, %0d/%0d frames (busy=%b wait_t=%b wait_p=%b t_cnt=%0d p_cnt=%0d)",
                     cyc, swaps, nframes, raster_busy, wait_teensy, wait_ps, t_cnt, p_cnt);
            errors = errors + 1;
        end
        // let things settle; no further frame may complete unexpectedly
        repeat (2000) @(posedge clk);
        if (swap_req) begin
            $display("TB NOTE: another frame completed after the expected %0d", nframes);
        end
        $display("COUNTERS overflow=%0d bad=%0d dropped=%0d axi_err=%0d prim_count=%0d sprite_beats=%0d last_fno=%0d ret_full=%0d ret_frame=%0d ring_stall=%0d",
                 list_overflow_cnt, bad_record_cnt, dropped_cnt, axi_err_cnt, prim_count, rd_beats_total,
                 last_frame_no, ret_full, ret_frame, ring_stall);
        $display("TB_RESULT errors=%0d frames=%0d cycles=%0d", errors, swaps, cyc);
        $finish;
    end
endmodule
