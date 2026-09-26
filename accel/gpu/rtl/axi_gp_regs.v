// ---------------------------------------------------------------------------------------------
// axi_gp_regs -- AXI3 slave on the PS M_AXI_GP0 port: register file + PS command FIFO push
//                (SPEC section 9, register map in common/gpu_proto.h / rtl/gpu_defs.vh)
//
// AXI3, 32-bit data, 12-bit IDs, address bits [11:0] decoded (the 4 KB window aliases above).
//
// Write path: one write burst at a time.
//   AWREADY = !aw_have (never depends on W), WREADY = aw_have && !bvalid. A W beat that arrives
//   before its AW simply waits (legal: the slave may wait for AW before asserting WREADY, and
//   AWREADY does not wait for W, so AW-before-W, W-before-AW and simultaneous all complete).
//   Every beat is its own register access at the AXI beat address: FIXED keeps the address,
//   INCR advances by 2**AWSIZE (aligned after the first beat), WRAP wraps at (len+1)*2**size.
//   The burst ends on WLAST; BVALID is raised with BID = AWID, BRESP = OKAY. The next AW is
//   accepted after the B handshake.
// Read path: one read burst at a time, ARREADY = !ar_have. Each beat's data is sampled into a
//   register (status inputs are "sampled when read"), presented with RID = ARID, RRESP = OKAY,
//   RLAST on beat ARLEN. 2 cycles per beat.
// All payloads are registers and stay stable while VALID && !READY.
//
// Registers (WSTRB honoured byte-wise on RW registers; a beat with WSTRB = 0 changes nothing):
//   0x008 CONTROL  bit0 SOFT_RESET: writing 1 emits a 1-cycle soft_reset pulse, reads as 0.
//                  bits 3:1 SRC_TEENSY/SRC_PS/SCANOUT_EN stored; reset value 0x00000004.
//   0x014 FB0 / 0x018 FB1: bits [31:12] stored, bits [11:0] read as 0 (framebuffers must be
//                  4 KB aligned so no strip/scanout burst can cross a 4 KB boundary).
//   0x020 CLEAR_COLOR [15:0].
//   0x04C RET_ADDR (SPEC 13.1): bits [31:12] stored (4 KB aligned), reset 0x1FE00000.
//   0x050 RET_CTRL: bit0 RET_ENABLE stored (reset 0); writing bit1 = 1 emits a 1-cycle ret_ack
//                  pulse (RET_ACK is write-only, reads 0).
//   0x054 RET_STATUS {ret_capturing, ret_full}, 0x058 RET_FRAME, 0x05C LAST_FRAME_NO: inputs.
//   0x100 PS_FIFO_DATA / 0x104 PS_FIFO_SOR (write-only, read 0): every beat with WSTRB != 0
//                  pushes {sor, wdata} (sor = 1 for 0x104) one cycle after the W handshake.
//                  The FIFO itself drops pushes while full (software checks PS_FIFO_FREE).
//   Read-only registers return the live inputs; unmapped addresses read 0, writes are ignored.
//
// NOTE: the RET_* registers and ports (ret_addr ... last_frame_no) come from common/gpu_proto.h /
// SPEC section 13 (offsets in rtl/gpu_defs.vh) and are not in rtl/INTERFACES.md yet; the port
// names match core_top's.
// ---------------------------------------------------------------------------------------------
`include "gpu_defs.vh"

module axi_gp_regs (
    input  wire        clk,
    input  wire        rst,
    // AXI3 slave: write address
    input  wire [31:0] awaddr,
    input  wire [3:0]  awlen,
    input  wire [2:0]  awsize,
    input  wire [1:0]  awburst,
    input  wire [11:0] awid,
    input  wire        awvalid,
    output wire        awready,
    // write data
    input  wire [31:0] wdata,
    input  wire [3:0]  wstrb,
    input  wire        wlast,
    input  wire [11:0] wid,
    input  wire        wvalid,
    output wire        wready,
    // write response
    output wire [1:0]  bresp,
    output wire [11:0] bid,
    output wire        bvalid,
    input  wire        bready,
    // read address
    input  wire [31:0] araddr,
    input  wire [3:0]  arlen,
    input  wire [2:0]  arsize,
    input  wire [1:0]  arburst,
    input  wire [11:0] arid,
    input  wire        arvalid,
    output wire        arready,
    // read data
    output wire [31:0] rdata,
    output wire [1:0]  rresp,
    output wire        rlast,
    output wire [11:0] rid,
    output wire        rvalid,
    input  wire        rready,
    // PS command FIFO write side
    output reg         pf_wr,
    output reg  [32:0] pf_din,
    input  wire [9:0]  pf_free,
    // control outputs
    output reg         soft_reset,
    output wire        src_teensy_en,
    output wire        src_ps_en,
    output wire        scanout_en,
    output wire [31:0] fb0_addr,
    output wire [31:0] fb1_addr,
    output wire [15:0] clear_color,
    // status inputs (sampled when read)
    input  wire        mmcm_locked,
    input  wire        raster_busy,
    input  wire        hpd,
    input  wire        teensy_active,
    input  wire        wait_teensy,
    input  wire        wait_ps,
    input  wire        swap_pending,
    input  wire [31:0] frame_count,
    input  wire        front_idx,
    input  wire [10:0] t_fifo_level,
    input  wire [31:0] list_overflow_cnt,
    input  wire [31:0] bad_record_cnt,
    input  wire [31:0] t_words,
    input  wire [31:0] render_cycles,
    input  wire [31:0] prim_count,
    input  wire [31:0] vsync_count,
    input  wire [31:0] axi_err_cnt,
    input  wire [31:0] dropped_cnt,
    // SPEC 13.1 return capture (not in rtl/INTERFACES.md yet)
    output wire [31:0] ret_addr,
    output wire        ret_enable,
    output reg         ret_ack,          // 1-cycle pulse
    input  wire        ret_full,
    input  wire        ret_capturing,
    input  wire [31:0] ret_frame,
    input  wire [31:0] last_frame_no
);
    // ---- beat address sequencing (AXI3 burst rules) -------------------------------------------
    // Only bits [11:0] are tracked: a legal burst never crosses a 4 KB boundary.
    function [11:0] next_addr;
        input [11:0] a;
        input [2:0]  size;
        input [1:0]  burst;
        input [3:0]  len;
        reg   [1:0]  sz;
        reg   [11:0] nbytes;
        reg   [11:0] aligned;
        reg   [11:0] wmask;
        begin
            sz      = (size > 3'd2) ? 2'd2 : size[1:0];        // 32-bit bus: at most 4 bytes
            nbytes  = 12'd1 << sz;
            aligned = a & ~(nbytes - 12'd1);
            wmask   = (({8'd0, len} + 12'd1) << sz) - 12'd1;   // wrap boundary - 1
            case (burst)
                2'b00:   next_addr = a;                                        // FIXED
                2'b10:   next_addr = (a & ~wmask) | ((aligned + nbytes) & wmask); // WRAP
                default: next_addr = aligned + nbytes;                         // INCR (and rsvd)
            endcase
        end
    endfunction

    // ---- registers ----------------------------------------------------------------------------
    localparam [31:0] CTRL_RST = `CONTROL_RESET;
    localparam [31:0] FB0_RST  = `FB0_RESET;
    localparam [31:0] FB1_RST  = `FB1_RESET;
    localparam [31:0] RET_RST  = `RET_ADDR_RESET;

    reg [3:1]  ctrl;          // [1] SRC_TEENSY, [2] SRC_PS, [3] SCANOUT_EN
    reg [31:12] fb0_hi, fb1_hi, ret_hi;
    reg [15:0] cc;
    reg        ret_en;

    assign src_teensy_en = ctrl[1];
    assign src_ps_en     = ctrl[2];
    assign scanout_en    = ctrl[3];
    assign fb0_addr      = {fb0_hi, 12'h000};
    assign fb1_addr      = {fb1_hi, 12'h000};
    assign clear_color   = cc;
    assign ret_addr      = {ret_hi, 12'h000};
    assign ret_enable    = ret_en;

    // ---- write channel ------------------------------------------------------------------------
    reg        aw_have;
    reg [11:0] aw_a;
    reg [3:0]  aw_len;
    reg [2:0]  aw_size;
    reg [1:0]  aw_burst;
    reg [11:0] aw_id;
    reg        b_valid;

    assign awready = !aw_have && !rst;
    assign wready  = aw_have && !b_valid;
    assign bvalid  = b_valid;
    assign bid     = aw_id;
    assign bresp   = 2'b00;

    wire aw_hs = awvalid && awready;
    wire w_hs  = wvalid && wready;
    wire b_hs  = b_valid && bready;

    wire [11:0] wreg = {aw_a[11:2], 2'b00};   // word address of the current write beat

    always @(posedge clk) begin
        soft_reset <= 1'b0;
        ret_ack    <= 1'b0;
        pf_wr      <= 1'b0;
        if (rst) begin
            aw_have  <= 1'b0;
            aw_a     <= 12'd0;
            aw_len   <= 4'd0;
            aw_size  <= 3'd0;
            aw_burst <= 2'b01;
            aw_id    <= 12'd0;
            b_valid  <= 1'b0;
            pf_din   <= 33'd0;
            ctrl     <= CTRL_RST[3:1];
            fb0_hi   <= FB0_RST[31:12];
            fb1_hi   <= FB1_RST[31:12];
            cc       <= 16'h0000;
            ret_hi   <= RET_RST[31:12];
            ret_en   <= 1'b0;
        end else begin
            if (aw_hs) begin
                aw_have  <= 1'b1;
                aw_a     <= awaddr[11:0];
                aw_len   <= awlen;
                aw_size  <= awsize;
                aw_burst <= awburst;
                aw_id    <= awid;
            end
            if (w_hs) begin
                // one register access per beat
                case (wreg)
                    `R_CONTROL: if (wstrb[0]) begin
                        ctrl       <= wdata[3:1];
                        soft_reset <= wdata[0];
                    end
                    `R_FB0: begin
                        if (wstrb[1]) fb0_hi[15:12] <= wdata[15:12];
                        if (wstrb[2]) fb0_hi[23:16] <= wdata[23:16];
                        if (wstrb[3]) fb0_hi[31:24] <= wdata[31:24];
                    end
                    `R_FB1: begin
                        if (wstrb[1]) fb1_hi[15:12] <= wdata[15:12];
                        if (wstrb[2]) fb1_hi[23:16] <= wdata[23:16];
                        if (wstrb[3]) fb1_hi[31:24] <= wdata[31:24];
                    end
                    `R_CLEAR_COLOR: begin
                        if (wstrb[0]) cc[7:0]  <= wdata[7:0];
                        if (wstrb[1]) cc[15:8] <= wdata[15:8];
                    end
                    `R_RET_ADDR: begin
                        if (wstrb[1]) ret_hi[15:12] <= wdata[15:12];
                        if (wstrb[2]) ret_hi[23:16] <= wdata[23:16];
                        if (wstrb[3]) ret_hi[31:24] <= wdata[31:24];
                    end
                    `R_RET_CTRL: if (wstrb[0]) begin
                        ret_en  <= wdata[0];
                        ret_ack <= wdata[1];
                    end
                    `R_PS_FIFO_DATA: if (|wstrb) begin
                        pf_wr  <= 1'b1;
                        pf_din <= {1'b0, wdata};
                    end
                    `R_PS_FIFO_SOR: if (|wstrb) begin
                        pf_wr  <= 1'b1;
                        pf_din <= {1'b1, wdata};
                    end
                    default: ;
                endcase
                aw_a <= next_addr(aw_a, aw_size, aw_burst, aw_len);
                if (wlast)
                    b_valid <= 1'b1;
            end
            if (b_hs) begin
                b_valid <= 1'b0;
                aw_have <= 1'b0;
            end
        end
    end

    // ---- read channel -------------------------------------------------------------------------
    reg        ar_have;
    reg [11:0] ar_a;
    reg [3:0]  ar_len;
    reg [3:0]  ar_left;       // beats remaining after the current one
    reg [2:0]  ar_size;
    reg [1:0]  ar_burst;
    reg [11:0] ar_id;
    reg        r_valid;
    reg        r_last;
    reg [31:0] r_data;

    assign arready = !ar_have && !rst;
    assign rvalid  = r_valid;
    assign rdata   = r_data;
    assign rlast   = r_last;
    assign rid     = ar_id;
    assign rresp   = 2'b00;

    wire ar_hs = arvalid && arready;
    wire r_hs  = r_valid && rready;

    reg [31:0] rd_mux;
    always @(*) begin
        case ({ar_a[11:2], 2'b00})
            `R_ID: rd_mux = `GPU_ID_VALUE;
            `R_VERSION: rd_mux = `GPU_VERSION_VALUE;
            `R_CONTROL: rd_mux = {28'd0, ctrl, 1'b0};
            `R_STATUS: rd_mux = {25'd0, swap_pending, wait_ps, wait_teensy,
                                               teensy_active, hpd, raster_busy, mmcm_locked};
            `R_FRAME_COUNT: rd_mux = frame_count;
            `R_FB0: rd_mux = {fb0_hi, 12'h000};
            `R_FB1: rd_mux = {fb1_hi, 12'h000};
            `R_FRONT: rd_mux = {31'd0, front_idx};
            `R_CLEAR_COLOR: rd_mux = {16'd0, cc};
            `R_PS_FIFO_FREE: rd_mux = {22'd0, pf_free};
            `R_T_FIFO_LEVEL: rd_mux = {21'd0, t_fifo_level};
            `R_LIST_OVERFLOW: rd_mux = list_overflow_cnt;
            `R_BAD_RECORDS: rd_mux = bad_record_cnt;
            `R_T_WORDS: rd_mux = t_words;
            `R_RENDER_CYCLES: rd_mux = render_cycles;
            `R_PRIM_COUNT: rd_mux = prim_count;
            `R_VSYNC_COUNT: rd_mux = vsync_count;
            `R_AXI_ERRORS: rd_mux = axi_err_cnt;
            `R_DROPPED: rd_mux = dropped_cnt;
            `R_RET_ADDR: rd_mux = {ret_hi, 12'h000};
            `R_RET_CTRL: rd_mux = {31'd0, ret_en};
            `R_RET_STATUS: rd_mux = {30'd0, ret_capturing, ret_full};
            `R_RET_FRAME: rd_mux = ret_frame;
            `R_LAST_FRAME_NO: rd_mux = last_frame_no;
            default:                 rd_mux = 32'd0;
        endcase
    end

    always @(posedge clk) begin
        if (rst) begin
            ar_have  <= 1'b0;
            ar_a     <= 12'd0;
            ar_len   <= 4'd0;
            ar_left  <= 4'd0;
            ar_size  <= 3'd0;
            ar_burst <= 2'b01;
            ar_id    <= 12'd0;
            r_valid  <= 1'b0;
            r_last   <= 1'b0;
            r_data   <= 32'd0;
        end else begin
            if (ar_hs) begin
                ar_have  <= 1'b1;
                ar_a     <= araddr[11:0];
                ar_len   <= arlen;
                ar_left  <= arlen;
                ar_size  <= arsize;
                ar_burst <= arburst;
                ar_id    <= arid;
            end else if (ar_have && !r_valid) begin
                // present the current beat (data sampled now, held until the handshake)
                r_valid <= 1'b1;
                r_data  <= rd_mux;
                r_last  <= (ar_left == 4'd0);
            end else if (r_hs) begin
                r_valid <= 1'b0;
                if (r_last) begin
                    ar_have <= 1'b0;
                end else begin
                    ar_a    <= next_addr(ar_a, ar_size, ar_burst, ar_len);
                    ar_left <= ar_left - 4'd1;
                end
            end
        end
    end

    // ---- unused inputs (lint) -----------------------------------------------------------------
    wire _unused_ok = &{1'b0, awaddr[31:12], araddr[31:12], wid, 1'b0};

endmodule
