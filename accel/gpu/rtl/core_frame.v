// core_frame.v -- frame / strip controller of the rasteriser.
//
// Frame: take a complete list, latch clear colour + back buffer base (front_idx ? fb0 : fb1),
// render strips 0..44, wait for the writer's last B response, raise swap_req, wait swap_done,
// free the list.
// Strip s (colour bank s&1):
//   1. wait until the writer has read + cleared strip s-2 from that colour bank,
//   2. if the bank is not known to be cleared with this frame's clear colour (after reset, or
//      when the clear colour changed since the bank was last cleared), clear it here
//      (2560 cycles of 128-bit words: colour = clear colour),
//   3. walk the list (core_fetch) and dispatch each record in order: TRI -> core_tri,
//      SPRITE -> core_sprite. TRI setup may overlap the previous TRI's span; sprites are fully
//      serialised against triangles (and core_tri drains its pipeline between primitives),
//   4. when the walk is done and all units are idle, hand the strip to the writer.
// Z buffer: ONE bank for all strips. Every Z word carries a tag (the strip index that last
// wrote it); core_tri treats a word whose tag is not the current strip as 0xFFFF (cleared), so
// no per-strip Z clear is needed. The tags are reset once per frame by a Z clear pass
// (zc_we/zc_addr, 2560 cycles, tag = 63) that runs after strip 44 while the writer drains the
// last strip (F_WAITB / F_SWAP / F_IDLE) and after reset; a frame starts only when it is done.
// Return capture (SPEC 13.1): when a list is taken with ret_enable = 1 and ret_full = 0, the
// writer also writes every strip to ret_addr + s*0xA000 (ret_capturing = 1). At the frame's
// last B response (the cycle swap_req rises): ret_full = 1, ret_frame = the list's frame_no.
// ret_ack (pulse) clears ret_full; ack while not full does nothing. last_frame_no = frame_no
// of the list most recently swapped to the front (updated on swap_done).
// The ret_enable / ret_ack inputs are only used as if-conditions, so if they are left
// unconnected (z/x in simulation, tied 0 by synthesis) capture simply stays off.
module core_frame (
    input  wire         clk,
    input  wire         rst,
    // list
    input  wire         lst_avail,
    input  wire [10:0]  lst_cnt,
    input  wire [31:0]  lst_fno,
    output wire         lst_take,
    output reg          lst_free,
    // config
    input  wire [15:0]  clear_color,
    input  wire [31:0]  fb0_addr,
    input  wire [31:0]  fb1_addr,
    input  wire         front_idx,
    // swap
    output reg          swap_req,
    input  wire         swap_done,
    // writer
    output reg          wr_frame_start,
    output reg  [31:0]  wr_base,
    output reg          wr_cap,
    output reg  [31:0]  wr_ret_base,
    output reg  [15:0]  fcol,
    output reg  [5:0]   strips_ready,
    input  wire [5:0]   rd_strip,
    input  wire         wr_frame_done,
    input  wire         bank_clr,
    input  wire         bank_clr_idx,
    // fetcher
    output reg          f_start,
    output reg  [5:0]   strip,
    output reg  [10:0]  nrec,
    input  wire         f_done,
    input  wire         stage_valid,
    /* verilator lint_off UNUSED */
    input  wire [31:0]  stage_w0,
    /* verilator lint_on UNUSED */
    output wire         stage_release,
    // triangle unit
    output wire         t_start,
    input  wire         t_setup_busy,
    input  wire         t_idle_all,
    // sprite unit
    output wire         s_start,
    input  wire         s_idle,
    // colour clear pass writes (colour = fcol x8, all lanes)
    output reg          cp_we,
    output reg  [11:0]  cp_addr,
    // Z tag clear pass writes (whole word: tag = 63, Z = FFFF x8)
    output reg          zc_we,
    output reg  [11:0]  zc_addr,
    // return capture (SPEC 13.1)
    input  wire [31:0]  ret_addr,
    input  wire         ret_enable,
    input  wire         ret_ack,
    output reg          ret_full,
    output wire         ret_capturing,
    output reg  [31:0]  ret_frame,
    output reg  [31:0]  last_frame_no,
    // status
    output wire         raster_busy,
    output reg  [31:0]  render_cycles,
    output reg  [31:0]  prim_count
);
    localparam F_IDLE = 3'd0, F_BANK = 3'd1, F_CLEAR = 3'd2, F_RUN = 3'd3,
               F_WAITB = 3'd4, F_SWAP = 3'd5;
    reg [2:0]  st;
    reg [1:0]  bank_ok;
    reg [15:0] bank_col0, bank_col1;
    reg [11:0] ck;
    reg        run_arm;      // first cycle of F_RUN (fetcher not yet started)
    reg [31:0] cycles;
    reg [31:0] fno;          // frame_no of the list being rendered
    reg        zc_pend;      // Z tags must be reset before the next frame starts
    reg [11:0] zk;

    wire b = strip[0];
    wire bank_free   = (strip < 6'd2) || ({1'b0, rd_strip} + 7'd1 >= {1'b0, strip});
    wire bank_good   = b ? (bank_ok[1] && (bank_col1 == fcol)) : (bank_ok[0] && (bank_col0 == fcol));

    wire [3:0] rtype = stage_w0[31:28];
    wire in_run = (st == F_RUN) && !run_arm;
    assign t_start = in_run && stage_valid && (rtype == 4'd1) && !t_setup_busy && s_idle;
    assign s_start = in_run && stage_valid && (rtype == 4'd2) && t_idle_all && s_idle;
    wire   other   = in_run && stage_valid && (rtype != 4'd1) && (rtype != 4'd2);
    assign stage_release = t_start || s_start || other;

    wire strip_done = in_run && f_done && t_idle_all && s_idle;

    wire   zc_ok       = (st == F_IDLE) || (st == F_WAITB) || (st == F_SWAP);  // Z bank unused
    assign lst_take    = (st == F_IDLE) && lst_avail && !zc_pend;
    assign raster_busy   = (st != F_IDLE);
    assign ret_capturing = wr_cap;

    always @(posedge clk) begin
        if (rst) begin
            st             <= F_IDLE;
            bank_ok        <= 2'b00;
            bank_col0      <= 16'd0;
            bank_col1      <= 16'd0;
            swap_req       <= 1'b0;
            lst_free       <= 1'b0;
            wr_frame_start <= 1'b0;
            wr_base        <= 32'd0;
            fcol           <= 16'd0;
            strips_ready   <= 6'd0;
            f_start        <= 1'b0;
            strip          <= 6'd0;
            nrec           <= 11'd0;
            cp_we          <= 1'b0;
            cp_addr        <= 12'd0;
            ck             <= 12'd0;
            run_arm        <= 1'b0;
            cycles         <= 32'd0;
            render_cycles  <= 32'd0;
            prim_count     <= 32'd0;
            fno            <= 32'd0;
            wr_cap         <= 1'b0;
            wr_ret_base    <= 32'd0;
            ret_full       <= 1'b0;
            ret_frame      <= 32'd0;
            last_frame_no  <= 32'd0;
            zc_pend        <= 1'b1;
            zk             <= 12'd0;
            zc_we          <= 1'b0;
            zc_addr        <= 12'd0;
        end else begin
            if (ret_ack && ret_full)
                ret_full <= 1'b0;
            lst_free       <= 1'b0;
            wr_frame_start <= 1'b0;
            f_start        <= 1'b0;
            cp_we          <= 1'b0;
            run_arm        <= 1'b0;
            cycles         <= cycles + 32'd1;
            zc_we          <= 1'b0;

            // Z tag clear pass (only while the rasteriser does not use the Z bank)
            if (zc_pend && zc_ok) begin
                zc_we   <= 1'b1;
                zc_addr <= zk;
                zk      <= (zk == 12'd2559) ? 12'd0 : zk + 12'd1;
                if (zk == 12'd2559)
                    zc_pend <= 1'b0;
            end

            // bank cleared by the writer (with this frame's clear colour)
            if (bank_clr) begin
                if (bank_clr_idx) begin
                    bank_ok[1] <= 1'b1;
                    bank_col1  <= fcol;
                end else begin
                    bank_ok[0] <= 1'b1;
                    bank_col0  <= fcol;
                end
            end

            case (st)
                F_IDLE: begin
                    if (lst_avail && !zc_pend) begin
                        fcol           <= clear_color;
                        wr_base        <= front_idx ? fb0_addr : fb1_addr;
                        nrec           <= lst_cnt;
                        fno            <= lst_fno;
                        if (ret_enable && !ret_full) begin
                            wr_cap      <= 1'b1;
                            wr_ret_base <= ret_addr;
                        end else begin
                            wr_cap      <= 1'b0;
                        end
                        wr_frame_start <= 1'b1;
                        strips_ready   <= 6'd0;
                        strip          <= 6'd0;
                        cycles         <= 32'd1;
                        st             <= F_BANK;
                    end
                end
                F_BANK: begin
                    if (bank_free && !wr_frame_start) begin
                        if (bank_good) begin
                            f_start <= 1'b1;
                            run_arm <= 1'b1;
                            st      <= F_RUN;
                        end else begin
                            ck <= 12'd0;
                            st <= F_CLEAR;
                        end
                    end
                end
                F_CLEAR: begin
                    cp_we   <= 1'b1;
                    cp_addr <= ck;
                    ck      <= ck + 12'd1;
                    if (ck == 12'd2559) begin
                        if (b) begin
                            bank_ok[1] <= 1'b1;
                            bank_col1  <= fcol;
                        end else begin
                            bank_ok[0] <= 1'b1;
                            bank_col0  <= fcol;
                        end
                        f_start <= 1'b1;
                        run_arm <= 1'b1;
                        st      <= F_RUN;
                    end
                end
                F_RUN: begin
                    if (strip_done) begin
                        strips_ready <= strips_ready + 6'd1;
                        if (strip == 6'd44) begin
                            st      <= F_WAITB;
                            zc_pend <= 1'b1;     // Z bank free: reset its tags for the next frame
                        end else begin
                            strip <= strip + 6'd1;
                            st    <= F_BANK;
                        end
                    end
                end
                F_WAITB: begin
                    if (wr_frame_done) begin
                        swap_req      <= 1'b1;
                        render_cycles <= cycles;
                        prim_count    <= {21'd0, nrec};
                        if (wr_cap) begin
                            ret_full  <= 1'b1;
                            ret_frame <= fno;
                            wr_cap    <= 1'b0;
                        end
                        st            <= F_SWAP;
                    end
                end
                default: begin // F_SWAP
                    if (swap_done) begin
                        swap_req      <= 1'b0;
                        lst_free      <= 1'b1;
                        last_frame_no <= fno;
                        st       <= F_IDLE;
                    end
                end
            endcase
        end
    end
endmodule
