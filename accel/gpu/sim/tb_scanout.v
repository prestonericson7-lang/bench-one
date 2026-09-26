// tb_scanout.v -- end-to-end test of rtl/scanout.v through the OSERDESE2/OBUFDS sim stubs.
//
// Everything is checked on the serial TMDS pads: the testbench samples the 4 lanes on every
// serial clock edge, word-aligns on the clock lane (0000011111, bit 0 first), decodes TMDS
// (independent decoder), and compares
//   * exact video timing (DE/HSYNC/VSYNC for every pixel clock, against a reference counter
//     locked on the first VSYNC falling edge; VSYNC edges aligned with the HSYNC leading edge),
//   * every active pixel of every frame against RGB565->RGB888 of the image in fb0/fb1
//     (images chosen so that no pixel of fb0 equals the same pixel of fb1: any tearing or
//     mixed frame is detected), the test pattern, or black,
//   * the clock lane on every symbol, tmds_n == ~tmds_p on every bit.
// A random-latency AXI3 slave (multiple outstanding bursts, random ARREADY/RVALID gaps,
// optional error-response injection (EXOKAY/SLVERR/DECERR, all must be counted), stall control)
// serves fb0 (image A) and fb1 (image B = ~A) and checks the reader's AXI behaviour (ARLEN,
// 128-B alignment, no 4 KB crossing, sequential bursts, whole frames, frame base == front
// buffer, ARVALID/ARADDR stability, more than one read outstanding at some point).
// VSYNC_COUNT is checked absolutely: at every frame end it must equal the number of VSYNC
// leading edges decoded from the TMDS stream so far.
//
// Defines: HD -> real 1280x720 CEA timing, FIFO 1024: power-up in test-pattern mode
//          (SCANOUT_EN resets to 0), frames 0-1 colour bars (square moves +1), SCANOUT_EN = 1
//          during frame 1, frame 2 = fb0, swap requested during frame 2, frames 3-4 = fb1.
//          MED -> 256x128 raster, FIFO 1024 (default depth), 32x32 pattern square, full scenario.
//          otherwise a small 128x48 raster (FIFO 256) with the full scenario:
//          startup, 4 swaps, test pattern (with a swap while in pattern mode), reader stall ->
//          underflow -> resync, dropped FIFO word -> unexpected sof -> resync, RRESP error
//          counting, VSYNC_COUNT, FRAME_COUNT, swap_done pulse/swap_pending semantics.
// Plusargs: +seed=N  +core_ps=N (core half period in ps, default 3361 = 148.75 MHz core, SPEC 2;
//           3360 = exactly 2x the TB pixel clock, i.e. phase-locked like the real MMCM outputs)
//           +verbose
`timescale 1ns/1ps
module tb_scanout;
`ifdef HD
    localparam H_ACTIVE = 1280, H_FP = 110, H_SYNC = 40, H_BP = 220;
    localparam V_ACTIVE = 720,  V_FP = 5,   V_SYNC = 5,  V_BP = 20;
    localparam FIFO_AW  = 10;
`elsif MED
    localparam H_ACTIVE = 256,  H_FP = 16,  H_SYNC = 16, H_BP = 32;
    localparam V_ACTIVE = 128,  V_FP = 3,   V_SYNC = 5,  V_BP = 10;
    localparam FIFO_AW  = 10;
`else
    localparam H_ACTIVE = 128,  H_FP = 12,  H_SYNC = 10, H_BP = 20;
    localparam V_ACTIVE = 48,   V_FP = 3,   V_SYNC = 4,  V_BP = 6;
    localparam FIFO_AW  = 8;
`endif
    localparam H_TOTAL  = H_ACTIVE + H_FP + H_SYNC + H_BP;
    localparam V_TOTAL  = V_ACTIVE + V_FP + V_SYNC + V_BP;
    localparam HS_START = H_ACTIVE + H_FP, HS_END = HS_START + H_SYNC;
    localparam VS_START = V_ACTIVE + V_FP, VS_END = VS_START + V_SYNC;
    localparam NPIX     = H_ACTIVE * V_ACTIVE;
    localparam NWORDS   = NPIX / 4;
    localparam NBURSTS  = NWORDS / 16;
    localparam FRAME_BYTES = NPIX * 2;
    localparam FRAME_CLKS  = H_TOTAL * V_TOTAL;
    localparam [31:0] FB0 = 32'h1E000000;
    localparam [31:0] FB1 = 32'h1E200000;
    // test pattern geometry (must match rtl/video_pixel.v)
    localparam BAR_W   = H_ACTIVE / 8;
    localparam SQ      = (H_ACTIVE >= 256 && V_ACTIVE >= 128) ? 32 : 4;
    localparam SQ_Y    = (V_ACTIVE - SQ) / 2;
    localparam SQX_MAX = H_ACTIVE - SQ;

    localparam CLS_NONE = 0, CLS_A = 1, CLS_B = 2, CLS_BLACK = 3, CLS_PAT = 4, CLS_BAD = 5;

    // SPEC 2: pixel 74.375 MHz (13.445 ns), serial 371.875 MHz (2.689 ns), core 148.75 MHz
    // (6.723 ns).  1 ps resolution: serial half 1.344 ns and pixel half exactly 5x that
    // (74.40 MHz, 0.03 % fast; the ratio 5:1 and the phase alignment are what matter).
    localparam real SER_HALF = 1.344;           // 371.9 MHz serial clock
    localparam real PIX_HALF = 6.720;           // exactly 5 x serial period (74.4 MHz)

    // ------------------------------------------------------------------ clocks / DUT
    reg  clk = 0, clk_pix = 0, clk_ser = 0;
    reg  rst = 1, rst_pix = 1;
    real core_half = 3.361;                     // 148.75 MHz core (SPEC 2, MMCM divide 5)
    always #(core_half) clk = ~clk;
    always #(PIX_HALF)  clk_pix = ~clk_pix;
    always #(SER_HALF)  clk_ser = ~clk_ser;

    reg  [31:0] fb0_addr = FB0, fb1_addr = FB1;
`ifdef HD
    reg         scanout_en = 1'b0;              // CONTROL reset value: SCANOUT_EN = 0 (bars)
`else
    reg         scanout_en = 1'b1;
`endif
    reg         swap_req = 1'b0;
    wire        swap_done, front_idx, swap_pending;
    wire [31:0] frame_count, vsync_count, axi_err_cnt;
    wire [31:0] araddr;
    wire [3:0]  arlen;
    wire        arvalid, rready;
    reg         arready = 1'b0;
    reg  [63:0] rdata = 64'd0;
    reg  [1:0]  rresp = 2'b00;
    reg         rlast = 1'b0, rvalid = 1'b0;
    wire [3:0]  tmds_p, tmds_n;

    scanout #(
        .H_ACTIVE(H_ACTIVE), .H_FP(H_FP), .H_SYNC(H_SYNC), .H_BP(H_BP),
        .V_ACTIVE(V_ACTIVE), .V_FP(V_FP), .V_SYNC(V_SYNC), .V_BP(V_BP),
        .FIFO_AW(FIFO_AW)
    ) dut (
        .clk(clk), .rst(rst), .clk_pix(clk_pix), .clk_ser(clk_ser), .rst_pix(rst_pix),
        .fb0_addr(fb0_addr), .fb1_addr(fb1_addr), .scanout_en(scanout_en),
        .swap_req(swap_req), .swap_done(swap_done), .front_idx(front_idx),
        .frame_count(frame_count), .vsync_count(vsync_count), .swap_pending(swap_pending),
        .rd_araddr(araddr), .rd_arlen(arlen), .rd_arvalid(arvalid), .rd_arready(arready),
        .rd_rdata(rdata), .rd_rresp(rresp), .rd_rlast(rlast), .rd_rvalid(rvalid),
        .rd_rready(rready), .axi_err_cnt(axi_err_cnt),
        .tmds_p(tmds_p), .tmds_n(tmds_n)
    );

    // ------------------------------------------------------------------ bookkeeping
    integer errors = 0;
    integer seed = 1;
    reg     verbose = 1'b0;

    task fail;
        input [8*96-1:0] msg;
        begin
            errors = errors + 1;
            if (errors <= 40)
                $display("ERROR @%0t: %0s", $time, msg);
        end
    endtask

    function integer rnd;             // uniform 0..n-1
        input integer n;
        begin
            rnd = ($random(seed) & 32'h7FFFFFFF) % n;
        end
    endfunction

    // ------------------------------------------------------------------ images
    function [15:0] hashpix;
        input integer idx;
        reg [31:0] h;
        begin
            h = idx * 32'h9E3779B1;
            h = h ^ (h >> 15);
            h = h * 32'h85EBCA77;
            h = h ^ (h >> 13);
            hashpix = h[15:0] ^ h[31:16];
            if (hashpix == 16'h0000 || hashpix == 16'hFFFF) hashpix = 16'h5AA5;
        end
    endfunction

    function [15:0] imgpix;           // which: 1 = image A (fb0), 2 = image B (fb1) = ~A
        input integer which;
        input integer idx;
        begin
            imgpix = (which == CLS_A) ? hashpix(idx) : ~hashpix(idx);
        end
    endfunction

    function [23:0] expand565;
        input [15:0] p;
        begin
            expand565 = {p[15:11], p[15:13], p[10:5], p[10:9], p[4:0], p[4:2]};
        end
    endfunction

    function [23:0] patpix;
        input integer x, y, sqx;
        integer bi;
        begin
            if (sqx >= 0 && x >= sqx && x < sqx + SQ && y >= SQ_Y && y < SQ_Y + SQ)
                patpix = 24'h808080;
            else if (x == 0 || x == H_ACTIVE - 1 || y == 0 || y == V_ACTIVE - 1)
                patpix = 24'hFFFFFF;
            else begin
                bi = x / BAR_W;
                if (bi > 7) bi = 7;
                case (bi)
                    0: patpix = 24'hFFFFFF;
                    1: patpix = 24'hFFFF00;
                    2: patpix = 24'h00FFFF;
                    3: patpix = 24'h00FF00;
                    4: patpix = 24'hFF00FF;
                    5: patpix = 24'hFF0000;
                    6: patpix = 24'h0000FF;
                    default: patpix = 24'h000000;
                endcase
            end
        end
    endfunction

    function [63:0] memword;
        input [31:0] a;
        integer which, idx;
        begin
            which = 0; idx = 0;
            if (a >= FB0 && a < FB0 + FRAME_BYTES) begin which = CLS_A; idx = (a - FB0) / 2; end
            else if (a >= FB1 && a < FB1 + FRAME_BYTES) begin which = CLS_B; idx = (a - FB1) / 2; end
            if (which == 0)
                memword = 64'hDEAD_BEEF_DEAD_BEEF;
            else
                memword = {imgpix(which, idx + 3), imgpix(which, idx + 2),
                           imgpix(which, idx + 1), imgpix(which, idx)};
        end
    endfunction

    // ------------------------------------------------------------------ AXI3 slave (HP0 model)
    localparam QN = 16;
    reg  [31:0] q_addr [0:QN-1];
    integer     q_t    [0:QN-1];
    integer     q_head = 0, q_tail = 0, q_cnt = 0, q_beat = 0;
    integer     cyc = 0;
    reg         stall = 1'b0;
    integer     lat_min = 2, lat_max = 60, gap_pct = 25, ardy_pct = 70, err_pm = 3, qmax = 6;
    integer     n_err_beats = 0, n_beats = 0, n_ars = 0, max_q = 0;
    integer     n_resp [0:3];
    initial begin n_resp[0] = 0; n_resp[1] = 0; n_resp[2] = 0; n_resp[3] = 0; end
    reg         prev_arvalid = 1'b0, prev_arready = 1'b0;
    reg  [31:0] prev_araddr = 32'd0;
    reg  [31:0] exp_next_ar = 32'd0;
    reg         ar_started = 1'b0;
    integer     bursts_in_frame = 0, reader_frames = 0;
    reg         drop_arm = 1'b0;
    reg  [31:0] drop_addr = 32'd0;
    event       drop_ev;
    reg  [31:0] a_tmp;
    reg  [8*96-1:0] msg;

    always @(posedge clk) begin
        cyc = cyc + 1;
        if (!rst) begin
            if (rready !== 1'b1) fail("RREADY not 1");
            // ---- AR channel rules
            if (prev_arvalid && !prev_arready) begin
                if (arvalid !== 1'b1) fail("ARVALID dropped without handshake");
                else if (araddr !== prev_araddr) fail("ARADDR changed while ARVALID && !ARREADY");
            end
            if (arvalid === 1'bx) fail("ARVALID is X");
            if (arvalid && arready) begin
                n_ars = n_ars + 1;
                if (arlen !== 4'd15) fail("ARLEN != 15");
                if (araddr[6:0] !== 7'd0) fail("ARADDR not 128-byte aligned");
                if ({20'd0, araddr[11:0]} + 32'd8 * (arlen + 32'd1) > 32'd4096)
                    fail("read burst crosses a 4 KB boundary");
                if (araddr == FB0 || araddr == FB1) begin
                    if (ar_started && bursts_in_frame != NBURSTS) begin
                        $sformat(msg, "reader frame had %0d bursts (expected %0d)", bursts_in_frame, NBURSTS);
                        fail(msg);
                    end
                    if (araddr != (front_idx ? FB1 : FB0)) fail("reader frame base is not the front buffer");
                    bursts_in_frame = 1;
                    ar_started = 1'b1;
                    reader_frames = reader_frames + 1;
                end else begin
                    if (!ar_started || araddr != exp_next_ar) begin
                        $sformat(msg, "non-sequential AR %h (expected %h)", araddr, exp_next_ar);
                        fail(msg);
                    end
                    bursts_in_frame = bursts_in_frame + 1;
                end
                exp_next_ar = araddr + 32'd128;
                if (q_cnt >= QN) fail("slave queue overflow (TB)");
                q_addr[q_tail] = araddr;
                q_t[q_tail]    = cyc + lat_min + rnd(lat_max - lat_min + 1);
                q_tail = (q_tail + 1) % QN;
                q_cnt  = q_cnt + 1;
                if (q_cnt > max_q) max_q = q_cnt;
            end
            // ---- R channel
            if (rvalid && rready) begin
                n_beats = n_beats + 1;
                n_resp[rresp] = n_resp[rresp] + 1;
                if (rresp != 2'b00) n_err_beats = n_err_beats + 1;
                q_beat = q_beat + 1;
                if (q_beat == 16) begin
                    q_beat = 0;
                    q_head = (q_head + 1) % QN;
                    q_cnt  = q_cnt - 1;
                end
            end
            if (!rvalid || rready) begin
                if (q_cnt > 0 && cyc >= q_t[q_head] && !stall && rnd(100) >= gap_pct) begin
                    a_tmp = q_addr[q_head] + 8 * q_beat;
                    rvalid <= 1'b1;
                    rdata  <= memword(a_tmp);
                    rlast  <= (q_beat == 15);
                    // non-OKAY responses: EXOKAY, SLVERR, DECERR (SPEC 9: all count)
                    rresp  <= (rnd(1000) < err_pm) ? (2'd1 + rnd(3)) : 2'b00;
                    if (drop_arm && a_tmp == drop_addr) begin
                        drop_arm = 1'b0;
                        -> drop_ev;
                    end
                end else begin
                    rvalid <= 1'b0;
                    rlast  <= 1'b0;
                    rdata  <= {32'hBAD0BAD0, cyc};   // garbage while !RVALID
                end
            end
            arready <= !stall && (q_cnt < qmax) && (rnd(100) < ardy_pct);
            prev_arvalid = arvalid;
            prev_arready = arready;
            prev_araddr  = araddr;
        end
    end

    // Dropped FIFO write (simulates a lost word -> the video meets the next frame's sof early).
    integer drops_done = 0;
    always @(drop_ev) begin
        #0.1;
        force dut.fifo_wr = 1'b0;
        @(posedge clk);
        #0.1;
        release dut.fifo_wr;
        drops_done = drops_done + 1;
    end

    // ------------------------------------------------------------------ swap monitor (core)
    reg     prev_front = 1'b0, prev_swap_done = 1'b0, prev_swap_req = 1'b0;
    reg [31:0] prev_fc = 32'd0;
    integer swaps_seen = 0, last_swap_frame = -1;
    integer nframes = 0;              // completed video frames (checker)

    always @(posedge clk) begin
        if (!rst) begin
            if ((front_idx !== prev_front) !== swap_done) fail("front_idx changed without swap_done (or vice versa)");
            if (frame_count !== prev_fc + (swap_done ? 1 : 0)) fail("frame_count not incremented exactly on swap_done");
            if (swap_done && prev_swap_done) fail("swap_done longer than 1 cycle");
            if (swap_done && !prev_swap_req) fail("swap_done without swap_req");
            if (swap_done) begin
                swaps_seen = swaps_seen + 1;
                last_swap_frame = nframes;
                // the old front must not be read any more: nothing outstanding at the swap
                if (q_cnt != 0 || prev_arvalid) fail("reads outstanding when swap_done was raised");
                if (verbose) $display("[%0t] swap_done #%0d front=%0d frame_count=%0d (video frame %0d)",
                                      $time, swaps_seen, front_idx, frame_count, nframes);
            end
            if (swap_done && swap_pending) fail("swap_pending still set with swap_done");
        end
        prev_front     = front_idx;
        prev_swap_done = swap_done;
        prev_swap_req  = swap_req;
        prev_fc        = frame_count;
    end

    // ------------------------------------------------------------------ TMDS capture
    function [10:0] tdec;             // {is_ctrl, c1, c0, data[7:0]}
        input [9:0] s;
        reg [7:0] t, o;
        integer k;
        begin
            case (s)
                10'b1101010100: tdec = {1'b1, 2'd0, 8'd0};
                10'b0010101011: tdec = {1'b1, 2'd1, 8'd0};
                10'b0101010100: tdec = {1'b1, 2'd2, 8'd0};
                10'b1010101011: tdec = {1'b1, 2'd3, 8'd0};
                default: begin
                    t = s[9] ? ~s[7:0] : s[7:0];
                    o[0] = t[0];
                    for (k = 1; k < 8; k = k + 1)
                        o[k] = s[8] ? (t[k] ^ t[k-1]) : ~(t[k] ^ t[k-1]);
                    tdec = {1'b0, 2'd0, o};
                end
            endcase
        end
    endfunction

    reg  [9:0] w0 = 0, w1 = 0, w2 = 0, w3 = 0;
    reg        aligned = 1'b0;
    integer    bitcnt = 0, nsym = 0, nbits = 0, diff_errs = 0;

    // per-frame checker state
    reg        tlock = 1'b0, prev_vs = 1'b0;
    integer    rh = 0, rv = 0;
    reg        f_active = 1'b0;
    integer    f_cls = CLS_NONE, f_trunc = -1, f_shift = 0, f_shift_at = -1, f_errs = 0, f_sqx = -1;
    integer    npix_checked = 0, timing_errs = 0;
    localparam MAXF = 4096;
    integer    rec_cls [0:MAXF-1];
    integer    rec_trunc [0:MAXF-1];
    integer    rec_shift_at [0:MAXF-1];
    integer    rec_errs [0:MAXF-1];
    integer    rec_sqx [0:MAXF-1];
    reg [31:0] rec_vsc [0:MAXF-1];
    integer    rec_vsr [0:MAXF-1];    // VSYNC leading edges decoded from TMDS before the frame end
    reg        rec_chk [0:MAXF-1];    // frame covered by a scenario assertion
    reg        vs_last = 1'b0;
    integer    n_vs_rise = 0;

    task ferr;
        input integer x, y;
        input [23:0] got, exp;
        begin
            f_errs = f_errs + 1;
            errors = errors + 1;
            if (errors <= 40)
                $display("ERROR @%0t: frame %0d cls %0d pixel (%0d,%0d) got %06h expected %06h",
                         $time, nframes, f_cls, x, y, got, exp);
        end
    endtask

    task pixel;
        input integer x, y;
        input [23:0] rgb;
        integer idx;
        reg [23:0] e;
        begin
            idx = y * H_ACTIVE + x;
            if (idx == 0) begin
                f_active = 1'b1; f_trunc = -1; f_shift = 0; f_shift_at = -1; f_errs = 0; f_sqx = -1;
                if (rgb == expand565(imgpix(CLS_A, 0)))      f_cls = CLS_A;
                else if (rgb == expand565(imgpix(CLS_B, 0))) f_cls = CLS_B;
                else if (rgb == 24'd0)                       f_cls = CLS_BLACK;
                else if (rgb == patpix(0, 0, -1))            f_cls = CLS_PAT;
                else begin
                    f_cls = CLS_BAD;
                    ferr(x, y, rgb, 24'hxxxxxx);
                end
            end
            npix_checked = npix_checked + 1;
            if (f_active) begin
                case (f_cls)
                    CLS_A, CLS_B: begin
                        if (f_trunc >= 0) begin
                            if (rgb != 24'd0) ferr(x, y, rgb, 24'd0);
                        end else if (idx + f_shift < NPIX && rgb == expand565(imgpix(f_cls, idx + f_shift))) begin
                            // ok
                        end else if (idx % 4 == 0 && rgb == 24'd0) begin
                            f_trunc = idx;                          // black until the frame end
                        end else if (idx % 4 == 0 && f_shift == 0 && idx + 4 < NPIX &&
                                     rgb == expand565(imgpix(f_cls, idx + 4))) begin
                            f_shift = 4; f_shift_at = idx;          // one word lost
                        end else begin
                            e = (idx + f_shift < NPIX) ? expand565(imgpix(f_cls, idx + f_shift)) : 24'hxxxxxx;
                            ferr(x, y, rgb, e);
                        end
                    end
                    CLS_BLACK: if (rgb != 24'd0) ferr(x, y, rgb, 24'd0);
                    CLS_PAT: begin
                        e = patpix(x, y, f_sqx);
                        if (rgb != e && y == SQ_Y && f_sqx < 0 && rgb == 24'h808080) begin
                            f_sqx = x;
                            e = patpix(x, y, f_sqx);
                        end
                        if (rgb != e) ferr(x, y, rgb, e);
                    end
                    default: ;
                endcase
            end
        end
    endtask

    task end_frame;
        begin
            if (f_active) begin
                if (f_cls == CLS_PAT && f_sqx < 0) begin
                    f_errs = f_errs + 1;
                    fail("test pattern frame without the moving square");
                end
                if (nframes < MAXF) begin
                    rec_cls[nframes]      = f_cls;
                    rec_trunc[nframes]    = f_trunc;
                    rec_shift_at[nframes] = f_shift_at;
                    rec_errs[nframes]     = f_errs;
                    rec_sqx[nframes]      = f_sqx;
                    rec_vsc[nframes]      = vsync_count;
                    rec_vsr[nframes]      = n_vs_rise;
                    rec_chk[nframes]      = 1'b0;
                end
                if (verbose)
                    $display("[%0t] video frame %0d: cls=%0d trunc=%0d shift_at=%0d sqx=%0d errs=%0d",
                             $time, nframes, f_cls, f_trunc, f_shift_at, f_sqx, f_errs);
                nframes = nframes + 1;
                f_active = 1'b0;
            end
        end
    endtask

    task do_symbol;
        reg [10:0] d0, d1, d2;
        reg        is_ctrl, hs, vs, e_de, e_hs, e_vs;
        begin
            nsym = nsym + 1;
            if (w3 !== 10'b0000011111) fail("clock lane symbol is not 0000011111");
            d0 = tdec(w0); d1 = tdec(w1); d2 = tdec(w2);
            if (d0[10] !== d1[10] || d0[10] !== d2[10]) fail("lanes disagree on control/data period");
            is_ctrl = d0[10];
            hs = d0[8];
            vs = d0[9];
            // VSYNC leading edges on the wire (VSYNC only changes during blanking)
            if (is_ctrl) begin
                if (vs && !vs_last) n_vs_rise = n_vs_rise + 1;
                vs_last = vs;
            end
            if (!tlock) begin
                if (is_ctrl && prev_vs && !vs) begin
                    tlock = 1'b1; rh = HS_START; rv = VS_END;
                end
                prev_vs = is_ctrl ? vs : 1'b0;
            end
            if (tlock) begin
                e_de = (rh < H_ACTIVE) && (rv < V_ACTIVE);
                e_hs = (rh >= HS_START) && (rh < HS_END);
                e_vs = ((rv > VS_START) || (rv == VS_START && rh >= HS_START)) &&
                       ((rv < VS_END)   || (rv == VS_END   && rh <  HS_START));
                if (is_ctrl === e_de) begin
                    timing_errs = timing_errs + 1;
                    $sformat(msg, "DE mismatch at h=%0d v=%0d", rh, rv); fail(msg);
                end else if (is_ctrl) begin
                    if (hs !== e_hs || vs !== e_vs) begin
                        timing_errs = timing_errs + 1;
                        $sformat(msg, "sync mismatch at h=%0d v=%0d: hs=%b vs=%b exp %b %b", rh, rv, hs, vs, e_hs, e_vs);
                        fail(msg);
                    end
                    if (d1[9:8] !== 2'b00 || d2[9:8] !== 2'b00) fail("control token on lane 1/2 not 00");
                end else begin
                    pixel(rh, rv, {d2[7:0], d1[7:0], d0[7:0]});
                end
                if (rh == 0 && rv == V_ACTIVE) end_frame;
                if (rh == H_TOTAL - 1) begin
                    rh = 0;
                    rv = (rv == V_TOTAL - 1) ? 0 : rv + 1;
                end else begin
                    rh = rh + 1;
                end
            end
        end
    endtask

    always @(clk_ser) begin
        #(SER_HALF / 2.0);
        nbits = nbits + 1;
        if (tmds_n !== ~tmds_p) begin
            diff_errs = diff_errs + 1;
            if (diff_errs == 1) fail("tmds_n != ~tmds_p");
        end
        w0 = {tmds_p[0], w0[9:1]};
        w1 = {tmds_p[1], w1[9:1]};
        w2 = {tmds_p[2], w2[9:1]};
        w3 = {tmds_p[3], w3[9:1]};
        if (!aligned) begin
            // word alignment is acquired only after the DUT's pixel reset (clock lane checked
            // on every symbol from then on)
            if (!rst_pix && w3 === 10'b0000011111) begin
                aligned = 1'b1;
                bitcnt  = 0;
                do_symbol;
            end
        end else begin
            bitcnt = bitcnt + 1;
            if (bitcnt == 10) begin
                bitcnt = 0;
                do_symbol;
            end
        end
    end

    // ------------------------------------------------------------------ scenario helpers
    task wait_pix;
        input integer n;
        begin
            repeat (n) @(posedge clk_pix);
        end
    endtask

    task wait_frames_done;        // until nframes >= n
        input integer n;
        begin
            wait (nframes >= n);
        end
    endtask

    // Frame k must be a complete, correct frame of class cls.
    task expect_clean;
        input integer k, cls;
        input [8*40-1:0] what;
        begin
            rec_chk[k] = 1'b1;
            if (rec_cls[k] != cls || rec_trunc[k] != -1 || rec_shift_at[k] != -1 || rec_errs[k] != 0) begin
                $sformat(msg, "%0s: frame %0d cls=%0d trunc=%0d shift=%0d errs=%0d, expected clean cls %0d",
                         what, k, rec_cls[k], rec_trunc[k], rec_shift_at[k], rec_errs[k], cls);
                fail(msg);
            end
        end
    endtask

    // Frame k must be clean and of class c1 or c2.
    task expect_clean2;
        input integer k, c1, c2;
        input [8*40-1:0] what;
        begin
            if (rec_cls[k] == c2) expect_clean(k, c2, what);
            else                  expect_clean(k, c1, what);
        end
    endtask

    integer cur_img = CLS_A;       // image the display must show (after the swap settles)
    integer checked_upto = 0;      // frames [0, checked_upto) have been asserted

    // Raise swap_req at a random point, wait for swap_done, drop it 0..3 cycles later.
    // Returns the video frame in progress at swap_done in last_swap_frame.
    task do_swap;
        integer n0, t;
        begin
            n0 = swaps_seen;
            @(negedge clk);
            swap_req = 1'b1;
            @(negedge clk);
            if (!swap_pending && !swap_done && swaps_seen == n0) fail("swap_pending not set while swap_req waits");
            t = 0;
            while (swaps_seen == n0 && t < 3 * FRAME_CLKS * 2) begin
                @(negedge clk);
                t = t + 1;
            end
            if (swaps_seen == n0) fail("swap_done never came");
            if (swap_done) fail("swap_done still high one cycle later");
            repeat (rnd(4)) @(negedge clk);
            if (swap_pending) fail("swap_pending set after the swap while swap_req is held");
            swap_req = 1'b0;
            @(negedge clk);
            cur_img = (cur_img == CLS_A) ? CLS_B : CLS_A;
        end
    endtask

    // After a swap during video frame fs: frames < fs show the old image, fs old or new,
    // frames >= fs+1 the new one; all clean.  Checks [checked_upto, fs+2].
    task check_swap_frames;
        input integer fs;
        integer k, old_img;
        begin
            old_img = (cur_img == CLS_A) ? CLS_B : CLS_A;
            wait_frames_done(fs + 3);
            for (k = checked_upto; k < fs; k = k + 1) expect_clean(k, old_img, "before swap");
            expect_clean2(fs, old_img, cur_img, "swap frame");
            expect_clean(fs + 1, cur_img, "after swap +1");
            expect_clean(fs + 2, cur_img, "after swap +2");
            checked_upto = fs + 3;
        end
    endtask

    // ------------------------------------------------------------------ scenario
    integer k, fs, fp0, fp1, fu, fd, X, core_ps, n_pat, nshift, u0, b0, first_pat, last_pat;
    reg     in_pat;

    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        if ($value$plusargs("core_ps=%d", core_ps)) core_half = core_ps / 1000.0;
        verbose = $test$plusargs("verbose");
        $display("tb_scanout: %0dx%0d (total %0dx%0d), FIFO %0d words, seed %0d, core half period %0.3f ns",
                 H_ACTIVE, V_ACTIVE, H_TOTAL, V_TOTAL, 1 << FIFO_AW, seed, core_half);
        repeat (20) @(posedge clk);
        @(negedge clk) rst = 1'b0;
    end

    initial begin
        repeat (23) @(posedge clk_pix);
        @(negedge clk_pix) rst_pix = 1'b0;
    end

    initial begin : watchdog
`ifdef HD
        #(8.0 * FRAME_CLKS * 2.0 * PIX_HALF);
`else
        #(80.0 * FRAME_CLKS * 2.0 * PIX_HALF);
`endif
        $display("tb_scanout: FAIL (timeout, %0d frames seen)", nframes);
        $finish;
    end

    initial begin : scenario
        wait (!rst && !rst_pix);
`ifdef HD
        // ---- real 1280x720 timing.  Power-up with SCANOUT_EN = 0: frames 0 and 1 are colour
        //      bars; SCANOUT_EN = 1 during frame 1 (applied at the next vertical blanking), so
        //      frame 2 = fb0; swap requested during frame 2 -> frames 3 and 4 = fb1.
        wait_frames_done(1);
        wait (tlock && rv == 300 && rh == 0);
        @(negedge clk) scanout_en = 1'b1;
        wait_frames_done(2);
        expect_clean(0, CLS_PAT, "HD pattern frame 0");
        expect_clean(1, CLS_PAT, "HD pattern frame 1");
        if (rec_sqx[1] != ((rec_sqx[0] >= SQX_MAX) ? 0 : rec_sqx[0] + 1)) begin
            $sformat(msg, "HD: square x %0d -> %0d, not +1", rec_sqx[0], rec_sqx[1]);
            fail(msg);
        end
        $display("tb_scanout: HD pattern frames 0,1 clean, square x %0d -> %0d", rec_sqx[0], rec_sqx[1]);
        checked_upto = 2;
        wait (tlock && nframes == 2 && rv == 300 && rh == 0);
        do_swap;
        fs = last_swap_frame;
        if (fs != 2) fail("HD: swap expected during video frame 2");
        check_swap_frames(fs);
        if (rec_cls[2] != CLS_A) fail("HD: frame 2 (first framebuffer frame) is not fb0");
`else
        // ---- 1. start-up: first frames show fb0
        wait_frames_done(3);
        for (k = 0; k < 3; k = k + 1) expect_clean(k, CLS_A, "startup");
        checked_upto = 3;

        // ---- 2. swaps at random points of the frame
        repeat (4) begin
            wait_pix(rnd(FRAME_CLKS));
            do_swap;
            fs = last_swap_frame;
            check_swap_frames(fs);
        end
        if (frame_count != 4 || front_idx != 1'b0) fail("frame_count/front_idx after 4 swaps");

        // ---- 3. test pattern (with a swap while the pattern is shown)
        wait_pix(rnd(FRAME_CLKS));
        @(negedge clk) scanout_en = 1'b0;
        fp0 = nframes;                       // frame in progress: still the image
        wait_frames_done(fp0 + 3);
        do_swap;                             // cur_img flips while the pattern is displayed
        wait_frames_done(fp0 + 6);
        wait_pix(rnd(FRAME_CLKS));
        @(negedge clk) scanout_en = 1'b1;
        fp1 = nframes;                       // frame in progress: still the pattern
        wait_frames_done(fp1 + 3);
        // frames: [checked_upto, fp0] old image, fp0+1 image or pattern, fp0+2..fp1 pattern,
        //         fp1+1 pattern or new image, fp1+2 new image.
        for (k = checked_upto; k <= fp0; k = k + 1)
            expect_clean(k, (cur_img == CLS_A) ? CLS_B : CLS_A, "before pattern");
        expect_clean2(fp0 + 1, (cur_img == CLS_A) ? CLS_B : CLS_A, CLS_PAT, "pattern on");
        for (k = fp0 + 2; k <= fp1; k = k + 1) expect_clean(k, CLS_PAT, "pattern");
        expect_clean2(fp1 + 1, CLS_PAT, cur_img, "pattern off");
        expect_clean(fp1 + 2, cur_img, "after pattern");
        checked_upto = fp1 + 3;
        n_pat = 0; first_pat = -1; last_pat = -1;
        for (k = fp0 + 1; k <= fp1 + 1; k = k + 1) begin
            if (rec_cls[k] == CLS_PAT) begin
                n_pat = n_pat + 1;
                if (first_pat < 0) first_pat = k;
                last_pat = k;
                if (k > fp0 + 1 && rec_cls[k-1] == CLS_PAT &&
                    rec_sqx[k] != ((rec_sqx[k-1] >= SQX_MAX) ? 0 : rec_sqx[k-1] + 1)) begin
                    $sformat(msg, "square x %0d -> %0d (frames %0d,%0d) not +1", rec_sqx[k-1], rec_sqx[k], k-1, k);
                    fail(msg);
                end
            end
        end
        if (n_pat < 4) fail("fewer than 4 pattern frames");
        $display("tb_scanout: pattern frames %0d..%0d, square x %0d..%0d", first_pat, last_pat,
                 rec_sqx[first_pat], rec_sqx[last_pat]);

        // ---- 4. reader stall -> FIFO underflow mid-frame -> black -> resync at the next frame
        wait (tlock && rv == V_ACTIVE / 4 && rh == 0);
        fu = nframes;
        u0 = dut.u_pix.dbg_underflows;
        stall = 1'b1;
        wait_pix(((1 << FIFO_AW) * 4 / H_ACTIVE + 8) * H_TOTAL);
        stall = 1'b0;
        wait_frames_done(fu + 3);
        for (k = checked_upto; k < fu; k = k + 1) expect_clean(k, cur_img, "before underflow");
        rec_chk[fu] = 1'b1;
        if (rec_cls[fu] != cur_img || rec_trunc[fu] <= 0 || rec_trunc[fu] % 4 != 0 ||
            rec_shift_at[fu] != -1 || rec_errs[fu] != 0) begin
            $sformat(msg, "underflow frame %0d: cls=%0d trunc=%0d shift=%0d errs=%0d",
                     fu, rec_cls[fu], rec_trunc[fu], rec_shift_at[fu], rec_errs[fu]);
            fail(msg);
        end else
            $display("tb_scanout: underflow frame %0d black from pixel %0d (line %0d), next frames clean",
                     fu, rec_trunc[fu], rec_trunc[fu] / H_ACTIVE);
        expect_clean(fu + 1, cur_img, "after underflow +1");
        expect_clean(fu + 2, cur_img, "after underflow +2");
        checked_upto = fu + 3;
        if (dut.u_pix.dbg_underflows != u0 + 1) fail("dbg_underflows did not count exactly one underflow");

        // ---- 5. lost FIFO word -> unexpected sof at the end of the frame -> resync
        X = 16 + rnd(NWORDS - 48);
        b0 = dut.u_pix.dbg_bad_sof;
        wait_pix(rnd(FRAME_CLKS));
        drop_addr = (front_idx ? FB1 : FB0) + 8 * X;
        drop_arm = 1'b1;
        wait (drops_done == 1);
        fd = nframes;
        wait_frames_done(fd + 4);
        nshift = 0;
        for (k = checked_upto; k < fd + 4; k = k + 1) begin
            if (rec_shift_at[k] >= 0 && k >= fd) begin
                nshift = nshift + 1;
                rec_chk[k] = 1'b1;
                if (rec_cls[k] != cur_img || rec_shift_at[k] != 4 * X || rec_trunc[k] != NPIX - 4 ||
                    rec_errs[k] != 0) begin
                    $sformat(msg, "drop frame %0d: cls=%0d shift_at=%0d (exp %0d) trunc=%0d (exp %0d) errs=%0d",
                             k, rec_cls[k], rec_shift_at[k], 4 * X, rec_trunc[k], NPIX - 4, rec_errs[k]);
                    fail(msg);
                end else
                    $display("tb_scanout: word %0d dropped -> frame %0d shifted from pixel %0d, last word black, next frame clean",
                             X, k, rec_shift_at[k]);
            end else
                expect_clean(k, cur_img, "around word drop");
        end
        if (nshift != 1) fail("expected exactly one frame with a lost word");
        checked_upto = fd + 4;
        if (dut.u_pix.dbg_bad_sof != b0 + 1) fail("dbg_bad_sof did not count exactly one unexpected sof");

        // ---- 6. a few more swaps with fast back-to-back requests; unaligned fb addresses
        //         (low 7 bits must be ignored: bursts stay at the 128-byte aligned base)
        fb0_addr = FB0 + 32'h3F;
        fb1_addr = FB1 + 32'h45;
        repeat (2) begin
            wait_pix(rnd(FRAME_CLKS / 4));
            do_swap;
            fs = last_swap_frame;
            check_swap_frames(fs);
        end
`endif
        // ---- final checks
        err_pm = 0;
        repeat (500) @(posedge clk);
        @(negedge clk);
        if (axi_err_cnt != n_err_beats) begin
            $sformat(msg, "axi_err_cnt %0d != SLVERR beats sent %0d", axi_err_cnt, n_err_beats);
            fail(msg);
        end
        if (frame_count != swaps_seen) fail("frame_count != swaps");
        if (front_idx != (cur_img == CLS_B)) fail("front_idx does not match the displayed image");
        for (k = 1; k < nframes; k = k + 1)
            if (rec_vsc[k] != rec_vsc[k-1] + 1) begin
                $sformat(msg, "vsync_count %0d -> %0d between frames %0d and %0d", rec_vsc[k-1], rec_vsc[k], k-1, k);
                fail(msg);
            end
        for (k = 0; k < nframes; k = k + 1)
            if (rec_vsc[k] != rec_vsr[k]) begin
                $sformat(msg, "frame %0d end: vsync_count %0d != %0d VSYNC edges on the wire", k, rec_vsc[k], rec_vsr[k]);
                fail(msg);
            end
        if (nframes == 0 || rec_vsr[0] < 1) fail("no VSYNC edge decoded before frame 0 ended");
        if (max_q < 2) fail("the reader never had more than one read outstanding");
        if (n_resp[1] == 0 || n_resp[2] == 0 || n_resp[3] == 0)
            fail("not every non-OKAY RRESP code was injected (test setup)");
        for (k = 0; k < checked_upto; k = k + 1)
            if (!rec_chk[k]) begin
                $sformat(msg, "frame %0d was not covered by an assertion", k); fail(msg);
            end
        if (dut.u_ser0.u_master.stub_errors + dut.u_ser1.u_master.stub_errors +
            dut.u_ser2.u_master.stub_errors + dut.u_serc.u_master.stub_errors +
            dut.u_ser0.u_slave.stub_errors + dut.u_serc.u_slave.stub_errors != 0)
            fail("OSERDESE2 stub reported errors");
        if (timing_errs != 0) fail("timing errors");

        $display("tb_scanout: %0d video frames (%0d asserted), %0d pixels checked, %0d symbols, %0d serial bits/lane",
                 nframes, checked_upto, npix_checked, nsym, nbits);
        $display("tb_scanout: reader: %0d frames, %0d bursts, %0d beats (%0d non-OKAY: %0d EXOKAY %0d SLVERR %0d DECERR), max outstanding %0d; swaps %0d; vsync_count %0d (wire %0d); axi_err_cnt %0d",
                 reader_frames, n_ars, n_beats, n_err_beats, n_resp[1], n_resp[2], n_resp[3], max_q,
                 swaps_seen, vsync_count, n_vs_rise, axi_err_cnt);
        $display("tb_scanout: pixel side: synced frames %0d, black frames %0d, underflows %0d, unexpected sof %0d",
                 dut.u_pix.dbg_synced_frames, dut.u_pix.dbg_black_frames, dut.u_pix.dbg_underflows, dut.u_pix.dbg_bad_sof);
        if (errors == 0) $display("tb_scanout: PASS");
        else             $display("tb_scanout: FAIL (%0d errors)", errors);
        $finish;
    end
endmodule
