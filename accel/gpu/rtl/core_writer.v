// core_writer.v -- strip writer: colour strip buffers -> back framebuffer over AXI3 HP1.
//
// Frame = 45 strips x 320 bursts x 16 beats (128-byte aligned, never crossing 4 KB).
// Strip n of the frame lives in bank n&1 and goes to base + n*0xA000 (= base + burst*128).
//  * AW: issued for strip n once the rasteriser has handed it over (n < strips_ready).
//  * Reader: reads the 128-bit bank words (8 pixels = 2 AXI beats) in order, at most one every
//    other cycle (= 1 beat per cycle), into a small beat FIFO (low half first; credit: at most
//    16 beats issued and not yet sent on W). One cycle after each (final) read it clears that
//    word (colour = fcol x8) through the bank's write port, so the bank is clean for strip n+2.
//    When the last word of a strip has been read and cleared, rd_strip increments and
//    bank_clr pulses (the bank may be reused by the rasteriser from the next cycle).
//  * W: beats of burst k are only sent after AW k was accepted (never data before address).
//  * B: always accepted; frame_done = all B responses of the frame received.
//  * Return capture (SPEC 13.1, cap = 1 for the frame): every burst is written twice, first to
//    the back buffer, then to ret_base + same offset. The reader reads each 8-word (16-beat)
//    block twice (first pass without clear, second pass with clear), AW alternates back/ret, so
//    AW and W stay in the same order. 28800 bursts per frame instead of 14400.
module core_writer (
    input  wire         clk,
    input  wire         rst,
    input  wire         frame_start,     // pulse; previous frame fully done
    /* verilator lint_off UNUSED */
    input  wire [31:0]  base,            // back buffer base (128-byte aligned)
    /* verilator lint_on UNUSED */
    input  wire         cap,             // with frame_start: capture this frame to ret_base
    /* verilator lint_off UNUSED */
    input  wire [31:0]  ret_base,        // with frame_start
    /* verilator lint_on UNUSED */
    input  wire [5:0]   strips_ready,    // strips handed over by the rasteriser this frame
    output reg  [5:0]   rd_strip,        // strips read + cleared this frame
    output reg          bank_clr,        // pulse together with rd_strip increment
    output reg          bank_clr_idx,
    output wire         frame_done,
    // colour bank read (address to both banks)
    output reg          c_re,
    output reg  [11:0]  c_raddr,
    output wire         c_rbank,         // bank of the read presented with c_re
    input  wire [127:0] c_rdata0,
    input  wire [127:0] c_rdata1,
    // clear write (to bank clr_bank)
    output reg          clr_we,
    output reg          clr_bank,
    output reg  [11:0]  clr_addr,
    // AXI3 write master
    output reg  [31:0]  awaddr,
    output wire [3:0]   awlen,
    output reg          awvalid,
    input  wire         awready,
    output wire [63:0]  wdata,
    output wire         wlast,
    output wire         wvalid,
    input  wire         wready,
    input  wire         bvalid,
    output wire         bready
);
    // ------------------------------------------------------------------ frame config
    reg        capq;         // capturing this frame
    reg [14:0] frame_bursts;

    // ------------------------------------------------------------------ AW
    reg [5:0]  aw_strip;
    reg [8:0]  aw_k;
    reg        aw_half;      // capture: 0 = back buffer burst next, 1 = return buffer burst next
    reg [31:0] aw_next;      // back buffer address of burst k
    reg [31:0] aw_rnext;     // return buffer address of burst k

    // ------------------------------------------------------------------ reader
    reg [5:0]  rs;           // strip being read
    reg [11:0] rk;           // next word (0..2559)
    reg        rpass;        // capture: 0 = first pass of the current 8-word block
    reg [4:0]  occ;          // beats issued but not yet sent on W (<= 16)
    reg        iss_d;        // a read was issued in the previous cycle
    reg        hi_pend;      // high half of the word pushed last cycle still to push
    reg [63:0] hi_data;
    reg        rb0;          // bank of the read issued this cycle (with c_re)
    reg        last0;        // this read is the final read of its strip
    reg        clr0;         // this read clears its word
    reg        rv1, rv2, rb1, rb2;
    reg        clr_last;

    // ------------------------------------------------------------------ W / B
    reg [3:0]  wbeat;
    reg [4:0]  w_credit;     // bursts with AW accepted but W not finished (<= 9)
    reg [14:0] b_cnt;

    wire [63:0] fq;
    wire        fq_v;
    wire        w_hs = wvalid && wready;

    assign c_rbank    = rb0;
    assign awlen      = 4'd15;
    assign wvalid     = fq_v && (w_credit != 5'd0);
    assign wdata      = fq;
    assign wlast      = (wbeat == 4'd15);
    assign bready     = 1'b1;
    assign frame_done = (b_cnt == frame_bursts);

    wire issue = (rs < strips_ready) && (occ <= 5'd14) && !iss_d;
    wire [127:0] rd_word = rb2 ? c_rdata1 : c_rdata0;
    wire aw_hs = awvalid && awready;
    wire final_pass = !capq || rpass;       // this read is the one that clears the word

    // rv2 (word arrives: push the low half) and hi_pend (push the high half) never coincide:
    // reads are at least 2 cycles apart
    core_fifo #(.W(64), .AW(4)) u_wfifo (
        .clk(clk), .rst(rst),
        .wr(rv2 || hi_pend), .din(hi_pend ? hi_data : rd_word[63:0]),
        .rd(w_hs), .dout(fq), .valid(fq_v)
    );

    always @(posedge clk) begin
        if (rst) begin
            capq     <= 1'b0;
            frame_bursts <= 15'd14400;
            aw_strip <= 6'd0;
            aw_k     <= 9'd0;
            aw_half  <= 1'b0;
            aw_next  <= 32'd0;
            aw_rnext <= 32'd0;
            awaddr   <= 32'd0;
            awvalid  <= 1'b0;
            rs       <= 6'd0;
            rk       <= 12'd0;
            rpass    <= 1'b0;
            occ      <= 5'd0;
            iss_d    <= 1'b0;
            hi_pend  <= 1'b0;
            hi_data  <= 64'd0;
            c_re     <= 1'b0;
            c_raddr  <= 12'd0;
            rb0      <= 1'b0;
            last0    <= 1'b0;
            clr0     <= 1'b0;
            rv1      <= 1'b0;
            rv2      <= 1'b0;
            rb1      <= 1'b0;
            rb2      <= 1'b0;
            clr_we   <= 1'b0;
            clr_bank <= 1'b0;
            clr_addr <= 12'd0;
            clr_last <= 1'b0;
            rd_strip <= 6'd0;
            bank_clr <= 1'b0;
            bank_clr_idx <= 1'b0;
            wbeat    <= 4'd0;
            w_credit <= 5'd0;
            b_cnt    <= 15'd0;
        end else if (frame_start) begin
            capq     <= cap;
            frame_bursts <= cap ? 15'd28800 : 15'd14400;
            aw_strip <= 6'd0;
            aw_k     <= 9'd0;
            aw_half  <= 1'b0;
            aw_next  <= {base[31:7], 7'd0};
            aw_rnext <= {ret_base[31:7], 7'd0};
            awvalid  <= 1'b0;
            rs       <= 6'd0;
            rk       <= 12'd0;
            rpass    <= 1'b0;
            iss_d    <= 1'b0;
            rd_strip <= 6'd0;
            b_cnt    <= 15'd0;
            bank_clr <= 1'b0;
            c_re     <= 1'b0;
            clr_we   <= 1'b0;
        end else begin
            // ---------------- AW
            if (aw_hs)
                awvalid <= 1'b0;
            // at most 8 accepted-but-unwritten bursts (+1 being presented)
            if ((!awvalid || awready) && (aw_strip < strips_ready) && (w_credit < 5'd8)) begin
                awvalid <= 1'b1;
                if (capq && aw_half) begin
                    awaddr   <= aw_rnext;
                    aw_rnext <= aw_rnext + 32'd128;
                end else begin
                    awaddr  <= aw_next;
                    aw_next <= aw_next + 32'd128;
                end
                if (capq && !aw_half) begin
                    aw_half <= 1'b1;                 // same burst again, to the return buffer
                end else begin
                    aw_half <= 1'b0;
                    if (aw_k == 9'd319) begin
                        aw_k     <= 9'd0;
                        aw_strip <= aw_strip + 6'd1;
                    end else begin
                        aw_k <= aw_k + 9'd1;
                    end
                end
            end

            // ---------------- reader
            c_re  <= issue;
            iss_d <= issue;
            if (issue) begin
                c_raddr <= rk;
                rb0     <= rs[0];
                clr0    <= final_pass;
                last0   <= final_pass && (rk == 12'd2559);
                if (!final_pass) begin
                    // first (non-clearing) pass of a block: at its end re-read the block
                    if (rk[2:0] == 3'd7) begin
                        rpass <= 1'b1;
                        rk    <= {rk[11:3], 3'd0};
                    end else begin
                        rk <= rk + 12'd1;
                    end
                end else begin
                    if (capq && (rk[2:0] == 3'd7))
                        rpass <= 1'b0;
                    if (rk == 12'd2559) begin
                        rk <= 12'd0;
                        rs <= rs + 6'd1;
                    end else begin
                        rk <= rk + 12'd1;
                    end
                end
            end
            occ <= occ + (issue ? 5'd2 : 5'd0) - (w_hs ? 5'd1 : 5'd0);
            rv1 <= c_re;
            rv2 <= rv1;
            rb1 <= rb0;
            rb2 <= rb1;
            hi_pend <= rv2;
            if (rv2)
                hi_data <= rd_word[127:64];

            // clear the word read in the previous cycle (RAM sampled c_raddr at this edge)
            clr_we   <= c_re && clr0;
            clr_addr <= c_raddr;
            clr_bank <= rb0;
            clr_last <= c_re && last0;
            // the clear write of the last word lands at this edge -> bank free
            bank_clr     <= clr_we && clr_last;
            bank_clr_idx <= clr_bank;
            if (clr_we && clr_last)
                rd_strip <= rd_strip + 6'd1;

            // ---------------- W
            if (w_hs)
                wbeat <= wbeat + 4'd1;
            w_credit <= w_credit + (aw_hs ? 5'd1 : 5'd0) - ((w_hs && wlast) ? 5'd1 : 5'd0);

            // ---------------- B
            if (bvalid)
                b_cnt <= b_cnt + 15'd1;
        end
    end
endmodule
