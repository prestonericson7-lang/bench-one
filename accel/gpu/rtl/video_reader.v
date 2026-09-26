// video_reader.v -- scanout frame reader (core clock domain), part of scanout (SPEC 7, 8, 9).
//
// Fetches the front framebuffer (FRAME_WORDS 64-bit words, contiguous from the buffer base)
// with 128-byte aligned 16-beat INCR bursts on an AXI3 read port and pushes {sof, data64}
// into the scanout async FIFO; sof = 1 on the first word of every frame.
//
// * A burst is requested whenever the FIFO has room for it: words already in the FIFO (write
//   side view, never under-estimated) + beats requested but not yet received + 16 <= 2**FIFO_AW.
//   The check is registered (room_r, one cycle behind, conservative; see below) so the
//   FIFO-count arithmetic is not in the AR issue path.
//   Several reads may be outstanding; RREADY is constant 1 (room is reserved before the AR).
// * Frame start: after the last beat of a frame has been received (no read outstanding) the
//   reader starts the next frame.  At that moment, if swap_req is high (and has been seen low
//   since the previous swap), front_idx ^= 1, frame_count += 1 and swap_done pulses for one
//   cycle.  The buffer base (fb0/fb1 by the new front_idx, low 7 bits forced to 0) is latched
//   for the whole frame.  Because no read of the old frame is outstanding when the swap is
//   granted, the old front buffer is never read again after swap_done.
// * AXI: ARLEN = 15; ARSIZE/ARBURST/ARID/ARCACHE/... are tied off in gpu_top.  ARVALID is a
//   register that never depends on ARREADY; ARADDR is stable while ARVALID && !ARREADY.
//   Every beat with RRESP != OKAY increments axi_err_cnt (the data is still used).
module video_reader #(
    parameter FRAME_WORDS = 230400,       // 64-bit words per frame, multiple of 16
    parameter FIFO_AW     = 10
) (
    input  wire              clk,
    input  wire              rst,
    input  wire [31:0]       fb0_addr,
    input  wire [31:0]       fb1_addr,
    input  wire              swap_req,
    output reg               swap_done,
    output reg               front_idx,
    output reg  [31:0]       frame_count,
    output wire              swap_pending,
    // AXI3 read master
    output reg  [31:0]       rd_araddr,
    output wire [3:0]        rd_arlen,
    output reg               rd_arvalid,
    input  wire              rd_arready,
    input  wire [63:0]       rd_rdata,
    input  wire [1:0]        rd_rresp,
    input  wire              rd_rlast,
    input  wire              rd_rvalid,
    output wire              rd_rready,
    output reg  [31:0]       axi_err_cnt,
    // async FIFO write side
    output wire              fifo_wr,
    output wire [64:0]       fifo_din,
    input  wire [FIFO_AW:0]  fifo_count
);
    localparam NBURSTS = FRAME_WORDS / 16;
    localparam DEPTH   = 1 << FIFO_AW;
    localparam RW      = FIFO_AW + 2;           // width of the reservation arithmetic
    localparam [RW-1:0] R_BURST = 16;
    localparam [RW-1:0] R_ONE   = 1;
    localparam [RW-1:0] R_ZERO  = 0;
    localparam [RW-1:0] R_DEPTH = DEPTH;
    /* verilator lint_off WIDTH */
    localparam [23:0]   NB24    = NBURSTS;
    /* verilator lint_on WIDTH */

    // synthesis translate_off
    initial begin
        if (FRAME_WORDS % 16 != 0 || FRAME_WORDS == 0)
            $display("ERROR video_reader: FRAME_WORDS=%0d is not a positive multiple of 16", FRAME_WORDS);
        if (FIFO_AW < 5)
            $display("ERROR video_reader: FIFO_AW=%0d too small (need >= 5)", FIFO_AW);
    end
    // synthesis translate_on

    assign rd_arlen  = 4'd15;
    assign rd_rready = 1'b1;

    wire rbeat   = rd_rvalid;                   // rd_rready is constant 1
    wire ar_fire = rd_arvalid && rd_arready;

    reg          in_frame;                      // a frame is being fetched
    reg          swap_armed;                    // swap_req has been low since the last swap
    reg  [31:0]  base;                          // latched buffer base of the current frame
    reg  [31:0]  offset;                        // byte offset of the next burst
    reg  [23:0]  bursts_left;                   // bursts of this frame not yet requested
    reg  [RW-1:0] reserved;                     // beats requested (AR queued/sent) not received
    reg          sof_next;                      // next received beat is the frame's first word

    wire do_swap   = !in_frame && swap_req && swap_armed;
    wire next_front = front_idx ^ do_swap;
    wire [24:0] next_base = next_front ? fb1_addr[31:7] : fb0_addr[31:7];   // 128-byte units

    // FIFO room check, registered to keep the FIFO-count arithmetic out of the AR issue path
    // (6.72 ns core clock).  used = RAM words (write-side view, never under-estimated) + beats
    // owed by the slave.  From one cycle to the next, used grows only by 16 per issue (a received
    // beat moves one word from "owed" to "in the RAM"; pops only shrink it), so
    //   room_r(t+1) = (used(t) + 16 + (issue(t) ? 16 : 0) <= DEPTH)
    // guarantees used(t+1) + 16 <= DEPTH whenever room_r(t+1) = 1: a burst issued at t+1 always
    // fits (identical guarantee to the unregistered check, decided one cycle later).
    wire [RW-1:0] used      = {1'b0, fifo_count} + reserved;
    wire          room1     = (used <= R_DEPTH - R_BURST);             // room for one more burst
    wire          room2     = (used <= R_DEPTH - R_BURST - R_BURST);   // ... for two more
    reg           room_r;
    wire          can_issue = in_frame && (bursts_left != 24'd0) && room_r;
    wire          issue    = (!rd_arvalid || ar_fire) && can_issue;
    wire          frame_end = in_frame && (bursts_left == 24'd0) && !rd_arvalid && (reserved == R_ZERO);

    assign swap_pending = swap_req && swap_armed;
    assign fifo_wr      = rbeat;
    assign fifo_din     = {sof_next, rd_rdata};

    always @(posedge clk) begin
        if (rst) begin
            in_frame    <= 1'b0;
            swap_armed  <= 1'b1;
            swap_done   <= 1'b0;
            front_idx   <= 1'b0;
            frame_count <= 32'd0;
            base        <= 32'd0;
            offset      <= 32'd0;
            bursts_left <= 24'd0;
            reserved    <= {RW{1'b0}};
            room_r      <= 1'b0;
            sof_next    <= 1'b0;
            rd_arvalid  <= 1'b0;
            rd_araddr   <= 32'd0;
            axi_err_cnt <= 32'd0;
        end else begin
            // ---- frame start / swap ----
            swap_done <= do_swap;
            if (do_swap) begin
                front_idx   <= ~front_idx;
                frame_count <= frame_count + 32'd1;
                swap_armed  <= 1'b0;
            end else if (!swap_req) begin
                swap_armed  <= 1'b1;
            end

            if (!in_frame) begin
                in_frame    <= 1'b1;
                base        <= {next_base, 7'd0};
                offset      <= 32'd0;
                bursts_left <= NB24;
                sof_next    <= 1'b1;
            end else if (frame_end) begin
                in_frame    <= 1'b0;
            end

            // ---- address channel ----
            if (!rd_arvalid || ar_fire) begin
                if (can_issue) begin
                    rd_arvalid  <= 1'b1;
                    rd_araddr   <= base + offset;
                    offset      <= offset + 32'd128;
                    bursts_left <= bursts_left - 24'd1;
                end else begin
                    rd_arvalid  <= 1'b0;
                end
            end

            // ---- data channel ----
            reserved <= reserved + (issue ? R_BURST : R_ZERO) - (rbeat ? R_ONE : R_ZERO);
            room_r   <= issue ? room2 : room1;
            if (rbeat) begin
                sof_next <= 1'b0;
                if (rd_rresp != 2'b00)
                    axi_err_cnt <= axi_err_cnt + 32'd1;
            end
        end
    end

    // rd_rlast is not needed: beats are counted.  (Kept as a port for the AXI interface.)
    // fb addresses: bits [6:0] ignored (bursts must stay 128-byte aligned, SPEC 9).
    /* verilator lint_off UNUSED */
    wire unused_rlast = rd_rlast;
    wire [13:0] unused_fb_lo = {fb0_addr[6:0], fb1_addr[6:0]};
    /* verilator lint_on UNUSED */
endmodule
