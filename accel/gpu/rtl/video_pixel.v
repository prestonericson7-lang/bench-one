// video_pixel.v -- scanout pixel-domain front end (SPEC 8): video timing, FIFO consumption
// with SOF resynchronisation, test pattern, RGB565 -> RGB888.  Clock: clk_pix.
//
// Timing (defaults = CEA-861 1280x720p60): line = H_ACTIVE + H_FP + H_SYNC + H_BP pixels,
// frame = V_ACTIVE + V_FP + V_SYNC + V_BP lines, HSYNC/VSYNC active high.  HSYNC is high for
// pixels [H_ACTIVE+H_FP, H_ACTIVE+H_FP+H_SYNC).  VSYNC changes state together with the HSYNC
// leading edge (CEA-861 progressive convention): high from (line V_ACTIVE+V_FP, pixel
// H_ACTIVE+H_FP) to (line V_ACTIVE+V_FP+V_SYNC, same pixel), i.e. exactly V_SYNC line periods.
// After rst_pix the counters start at the first blanking line (line V_ACTIVE, pixel 0), so the
// reader has the vertical blanking interval to fill the FIFO before the first displayed frame.
//
// FIFO consumption ({sof, data64}, FWFT; pixel 4k of a word is bits [15:0]):
//  * "synced" = the current frame is being displayed from the FIFO.
//  * First active pixel of line 0: if the head word has sof -> pop it, synced = 1; otherwise the
//    frame is black (synced = 0) and a non-sof head is discarded.
//  * Synced, every 4th active pixel: pop the head.  If the FIFO is empty (underflow) or the
//    head has sof (unexpected sof mid-frame) -> synced = 0, black until the next frame start;
//    an sof word is not popped (it starts the next frame).
//  * Not synced (at any time, including blanking): discard non-sof words as they arrive and
//    keep an sof word at the head for the next line 0.  At the start of vertical blanking
//    synced is cleared, so leftovers of a short/long frame are discarded before the next line 0.
//
// Test pattern (scanout_en = 0; synchronised with 2 FFs and applied at the start of the next
// vertical blanking so frames are never mixed): 8 vertical bars of H_ACTIVE/8 pixels (white,
// yellow, cyan, green, magenta, red, blue, black), 1-pixel white border, and a grey (0x808080)
// SQxSQ square at line SQ_Y whose x advances by one pixel per video frame (0..H_ACTIVE-SQ,
// wraps).  Priority: square > border > bars.  The FIFO is consumed identically in both modes.
//
// Outputs are registered, aligned with each other: de/hsync/vsync/r/g/b (2 clocks after the
// counters).  vs_toggle toggles at every VSYNC leading edge (for VSYNC_COUNT in the core domain).
module video_pixel #(
    parameter H_ACTIVE = 1280,
    parameter H_FP     = 110,
    parameter H_SYNC   = 40,
    parameter H_BP     = 220,
    parameter V_ACTIVE = 720,
    parameter V_FP     = 5,
    parameter V_SYNC   = 5,
    parameter V_BP     = 20
) (
    input  wire        clk_pix,
    input  wire        rst_pix,
    input  wire        scanout_en,        // asynchronous (core domain level)
    // async FIFO read side (FWFT)
    input  wire        f_valid,
    input  wire [64:0] f_data,
    output reg         f_pop,
    // video out
    output reg         de,
    output reg         hsync,
    output reg         vsync,
    output reg  [7:0]  r,
    output reg  [7:0]  g,
    output reg  [7:0]  b,
    output reg         vs_toggle
);
    localparam H_TOTAL  = H_ACTIVE + H_FP + H_SYNC + H_BP;
    localparam V_TOTAL  = V_ACTIVE + V_FP + V_SYNC + V_BP;
    localparam HS_START = H_ACTIVE + H_FP;
    localparam HS_END   = HS_START + H_SYNC;
    localparam VS_START = V_ACTIVE + V_FP;
    localparam VS_END   = VS_START + V_SYNC;
    localparam BAR_W    = H_ACTIVE / 8;
    localparam SQ       = (H_ACTIVE >= 256 && V_ACTIVE >= 128) ? 32 : 4;
    localparam SQ_Y     = (V_ACTIVE - SQ) / 2;
    localparam SQX_MAX  = H_ACTIVE - SQ;

    localparam [11:0] C_H_TOTAL_M1 = H_TOTAL - 1;
    localparam [11:0] C_V_TOTAL_M1 = V_TOTAL - 1;
    localparam [11:0] C_H_ACTIVE   = H_ACTIVE;
    localparam [11:0] C_H_LAST     = H_ACTIVE - 1;
    localparam [11:0] C_V_ACTIVE   = V_ACTIVE;
    localparam [11:0] C_V_LAST     = V_ACTIVE - 1;
    localparam [11:0] C_HS_START   = HS_START;
    localparam [11:0] C_HS_END     = HS_END;
    localparam [11:0] C_VS_START   = VS_START;
    localparam [11:0] C_VS_END     = VS_END;
    localparam [11:0] C_BAR_W_M1   = BAR_W - 1;
    localparam [11:0] C_SQ         = SQ;
    localparam [11:0] C_SQ_Y       = SQ_Y;
    localparam [11:0] C_SQY_END    = SQ_Y + SQ;
    localparam [11:0] C_SQX_MAX    = SQX_MAX;

    // synthesis translate_off
    initial begin
        if (H_ACTIVE % 8 != 0 || H_ACTIVE < 16)
            $display("ERROR video_pixel: H_ACTIVE=%0d must be a multiple of 8 and >= 16", H_ACTIVE);
        if (H_TOTAL > 4095 || V_TOTAL > 4095)
            $display("ERROR video_pixel: timing too large for 12-bit counters");
        if (V_ACTIVE < 2 * SQ)
            $display("ERROR video_pixel: V_ACTIVE=%0d too small", V_ACTIVE);
    end
    // synthesis translate_on

    // ---------------- scanout_en synchroniser ----------------
    (* ASYNC_REG = "TRUE" *) reg en_s1;
    (* ASYNC_REG = "TRUE" *) reg en_s2;
    always @(posedge clk_pix) begin
        en_s1 <= scanout_en;
        en_s2 <= en_s1;
    end

    // ---------------- stage 0: counters ----------------
    reg [11:0] hc, vc;
    reg [11:0] bar_cnt;
    reg [2:0]  bar_idx;

    always @(posedge clk_pix) begin
        if (rst_pix) begin
            hc      <= 12'd0;
            vc      <= C_V_ACTIVE;
            bar_cnt <= 12'd0;
            bar_idx <= 3'd0;
        end else if (hc == C_H_TOTAL_M1) begin
            hc      <= 12'd0;
            vc      <= (vc == C_V_TOTAL_M1) ? 12'd0 : vc + 12'd1;
            bar_cnt <= 12'd0;
            bar_idx <= 3'd0;
        end else begin
            hc <= hc + 12'd1;
            if (bar_cnt == C_BAR_W_M1) begin
                bar_cnt <= 12'd0;
                if (bar_idx != 3'd7) bar_idx <= bar_idx + 3'd1;
            end else begin
                bar_cnt <= bar_cnt + 12'd1;
            end
        end
    end

    wire act0    = (hc < C_H_ACTIVE) && (vc < C_V_ACTIVE);
    wire wstart0 = act0 && (hc[1:0] == 2'b00);
    wire first0  = (hc == 12'd0) && (vc == 12'd0);
    wire fstart0 = (hc == 12'd0) && (vc == C_V_ACTIVE);      // start of vertical blanking

    // ---------------- FIFO consumption / SOF resync ----------------
    wire f_sof = f_data[64];
    reg  synced;
    reg  synced_nx;
    reg  word_ok;

    always @* begin
        f_pop     = 1'b0;
        word_ok   = 1'b0;
        synced_nx = synced;
        if (first0) begin
            if (f_valid && f_sof) begin
                f_pop = 1'b1; word_ok = 1'b1; synced_nx = 1'b1;
            end else begin
                f_pop = f_valid && !f_sof; synced_nx = 1'b0;
            end
        end else if (synced) begin
            if (wstart0) begin
                if (f_valid && !f_sof) begin
                    f_pop = 1'b1; word_ok = 1'b1;
                end else begin
                    synced_nx = 1'b0;               // underflow or unexpected sof
                end
            end
        end else begin
            f_pop = f_valid && !f_sof;              // seek: discard up to the next sof
        end
        if (fstart0)
            synced_nx = 1'b0;
    end

    reg [63:0] wrd;
    reg        wrd_ok;
    // status counters for debug / testbench observation (not used by the logic)
    reg [15:0] dbg_underflows, dbg_bad_sof, dbg_synced_frames, dbg_black_frames;

    always @(posedge clk_pix) begin
        if (rst_pix) begin
            synced <= 1'b0;
            wrd_ok <= 1'b0;
            dbg_underflows    <= 16'd0;
            dbg_bad_sof       <= 16'd0;
            dbg_synced_frames <= 16'd0;
            dbg_black_frames  <= 16'd0;
        end else begin
            synced <= synced_nx;
            if (wstart0)
                wrd_ok <= word_ok;
            if (synced && wstart0 && !first0 && !f_valid) dbg_underflows <= dbg_underflows + 16'd1;
            if (synced && wstart0 && !first0 && f_valid && f_sof) dbg_bad_sof <= dbg_bad_sof + 16'd1;
            if (first0 && word_ok)  dbg_synced_frames <= dbg_synced_frames + 16'd1;
            if (first0 && !word_ok) dbg_black_frames  <= dbg_black_frames + 16'd1;
        end
        if (wstart0)
            wrd <= f_data[63:0];
    end

    // ---------------- frame-rate state (changes only at the start of vertical blanking) ----
    reg        pat_mode;
    reg [11:0] sq_x;
    always @(posedge clk_pix) begin
        if (rst_pix) begin
            pat_mode  <= 1'b1;
            sq_x      <= 12'd0;
            vs_toggle <= 1'b0;
        end else begin
            if (fstart0) begin
                pat_mode <= !en_s2;
                sq_x     <= (sq_x >= C_SQX_MAX) ? 12'd0 : sq_x + 12'd1;
            end
            if (hc == C_HS_START && vc == C_VS_START)
                vs_toggle <= ~vs_toggle;
        end
    end

    // ---------------- stage 1 ----------------
    reg        act1, hs1, vs1;
    reg [11:0] hc1, vc1;
    reg [2:0]  bar1;
    always @(posedge clk_pix) begin
        if (rst_pix) begin
            act1 <= 1'b0;
            hs1  <= 1'b0;
            vs1  <= 1'b0;
        end else begin
            act1 <= act0;
            hs1  <= (hc >= C_HS_START) && (hc < C_HS_END);
            if (hc == C_HS_START)
                vs1 <= (vc >= C_VS_START) && (vc < C_VS_END);
        end
        hc1  <= hc;
        vc1  <= vc;
        bar1 <= bar_idx;
    end

    reg [15:0] px;
    always @* begin
        case (hc1[1:0])
            2'd0: px = wrd[15:0];
            2'd1: px = wrd[31:16];
            2'd2: px = wrd[47:32];
            default: px = wrd[63:48];
        endcase
    end

    wire in_sq  = (hc1 >= sq_x) && (hc1 < sq_x + C_SQ) && (vc1 >= C_SQ_Y) && (vc1 < C_SQY_END);
    wire border = (hc1 == 12'd0) || (hc1 == C_H_LAST) || (vc1 == 12'd0) || (vc1 == C_V_LAST);
    reg [23:0] bar_rgb;
    always @* begin
        case (bar1)
            3'd0: bar_rgb = 24'hFFFFFF;   // white
            3'd1: bar_rgb = 24'hFFFF00;   // yellow
            3'd2: bar_rgb = 24'h00FFFF;   // cyan
            3'd3: bar_rgb = 24'h00FF00;   // green
            3'd4: bar_rgb = 24'hFF00FF;   // magenta
            3'd5: bar_rgb = 24'hFF0000;   // red
            3'd6: bar_rgb = 24'h0000FF;   // blue
            default: bar_rgb = 24'h000000; // black
        endcase
    end
    wire [23:0] pat_rgb = in_sq ? 24'h808080 : border ? 24'hFFFFFF : bar_rgb;
    wire [23:0] fb_rgb  = {px[15:11], px[15:13], px[10:5], px[10:9], px[4:0], px[4:2]};

    // ---------------- stage 2 (outputs) ----------------
    always @(posedge clk_pix) begin
        if (rst_pix) begin
            de <= 1'b0; hsync <= 1'b0; vsync <= 1'b0;
            r  <= 8'd0; g <= 8'd0; b <= 8'd0;
        end else begin
            de    <= act1;
            hsync <= hs1;
            vsync <= vs1;
            if (!act1)
                {r, g, b} <= 24'd0;
            else if (pat_mode)
                {r, g, b} <= pat_rgb;
            else if (!wrd_ok)
                {r, g, b} <= 24'd0;
            else
                {r, g, b} <= fb_rgb;
        end
    end
endmodule
