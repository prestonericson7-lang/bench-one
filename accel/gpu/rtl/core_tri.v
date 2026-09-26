// core_tri.v -- triangle / rect unit: per-strip setup, span iterator, 8-pixel/clock pipeline.
//
// Values (v = 0..6): E0, E1, E2 (edge functions), z, r, g, b. For each: v0, dvdx, dvdy from the
// record (for edges: E0_i, A_i, B_i). All arithmetic is modulo 2^32 exactly as SPEC section 5:
//     V(x,y) = v0 + dvdx*(x-xmin) + dvdy*(y-ymin)
// Setup for strip rows [16s, 16s+15]: ys = max(ymin,16s), ye = min(ymax,16s+15),
//     acc = v0 + dvdy*(ys-ymin) - dvdx*(xmin & 7)
// (value at lane 0 of the 8-aligned group containing xmin, row ys). The multiply by
// dy0 = ys-ymin (< 1024) is an iterative shift-add (one bit per cycle, stops early), all 7
// values in parallel, 32-bit adders only. The multiples 3d, 5d, 7d of each dvdx are computed
// once per record (6d = 3d << 1).
// Span: each clock one 8-pixel group (one 128-bit strip word); lane j value = cur + j*dvdx,
// cur += 8*dvdx per group; next row start = previous row start + dvdy (kept in nrow).
//
// Pipeline (one group per clock):
//   S0  iterator output: cur values, bank address, lane mask         (Z read address issued)
//   S1  lane adders + coverage + z/colour clamp + RGB565 pack
//   S2  Z read data arrives (2-cycle RAM latency): tag check, Z test, merged Z word
//   S3  write enables / data registered; RAM writes land at the next edge
// Z bank word (144 bits) = { 10'b0, tag[5:0], z7 .. z0 }: the Z bank is shared by all strips; a
// word whose tag != strip holds stale data of another strip (or of the per-frame tag clear) and
// reads as 0xFFFF x8. With ZWRITE the whole word is written back: passing lanes get their new
// z, the other lanes their effective old value, tag = strip.
// A new primitive only enters S0 when S0..S3 are empty ("drain"), so a primitive's Z reads
// always see every Z write of all earlier primitives (no RMW hazard across primitives).
// Within one primitive every group address is visited at most once.
//
// Strip bank addressing: word = row * 160 + (x >> 3), pixel x & 7 in bits [16*(x&7) +: 16].
// Record bbox: rows ymin..min(ymax, strip end), cols xmin..min(xmax,1279). The caller only
// starts records whose side-table strip range contains `strip` (guarantees a non-empty span).
module core_tri (
    input  wire              clk,
    input  wire              rst,
    input  wire [5:0]        strip,
    // record from the stage buffer
    input  wire              start,          // pulse; only when !setup_busy
    input  wire [23*32-1:0]  rec,
    output wire              setup_busy,     // setup holds a record (computing or waiting)
    output wire              idle_all,       // setup idle, span idle, pipeline empty
    input  wire              go_ok,          // permission to launch the span (sprite unit idle)
    // Z bank read (single Z bank, see header)
    output wire              z_re,
    output wire [11:0]       z_raddr,
    input  wire [143:0]      z_rdata,
    // writes: colour to the strip bank (chosen outside), Z word to the Z bank
    output wire [7:0]        c_we,           // per pixel lane
    output wire              z_we,
    output wire [11:0]       w_addr,
    output wire [127:0]      c_wdata,
    output wire [143:0]      z_wdata,
    output wire              w_active        // S3 valid (for the write-port mux)
);
    // ------------------------------------------------------------------ record fields
    /* verilator lint_off UNUSED */
    wire [31:0] w0 = rec[0*32 +: 32];
    wire [31:0] w1 = rec[1*32 +: 32];
    /* verilator lint_on UNUSED */
    // v order: E0, E1, E2, z, r, g, b  (value v at bits [v*32 +: 32])
    wire [223:0] rv0 = {rec[20*32 +: 32], rec[17*32 +: 32], rec[14*32 +: 32], rec[11*32 +: 32],
                        rec[10*32 +: 32], rec[ 7*32 +: 32], rec[ 4*32 +: 32]};
    wire [223:0] rdx = {rec[21*32 +: 32], rec[18*32 +: 32], rec[15*32 +: 32], rec[12*32 +: 32],
                        rec[ 8*32 +: 32], rec[ 5*32 +: 32], rec[ 2*32 +: 32]};
    wire [223:0] rdy = {rec[22*32 +: 32], rec[19*32 +: 32], rec[16*32 +: 32], rec[13*32 +: 32],
                        rec[ 9*32 +: 32], rec[ 6*32 +: 32], rec[ 3*32 +: 32]};

    // geometry of the record for this strip
    wire [10:0] xmin = w0[10:0];
    wire [10:0] xmax = w0[21:11];
    wire [10:0] ymin = w1[10:0];
    wire [10:0] ymax = w1[21:11];
    wire [10:0] y0s  = {1'b0, strip, 4'b0000};
    wire [10:0] y1s  = {1'b0, strip, 4'b1111};
    wire [10:0] ys   = (ymin > y0s) ? ymin : y0s;
    /* verilator lint_off UNUSED */
    wire [10:0] ye   = (ymax < y1s) ? ymax : y1s;
    wire [10:0] xe   = (xmax > 11'd1279) ? 11'd1279 : xmax;
    wire [10:0] dy0  = ys - ymin;
    /* verilator lint_on UNUSED */

    // ------------------------------------------------------------------ setup state
    localparam SS_IDLE = 2'd0, SS_MUL = 2'd1, SS_XOFF = 2'd2, SS_READY = 2'd3;
    reg [1:0]   sst;
    reg [223:0] acc, mm, sdx, sdx3, sdx5, sdx7, sdy;
    reg [9:0]   mb;
    reg [2:0]   p_xo;
    reg [7:0]   p_gs, p_ge;
    reg [7:0]   p_mf, p_ml;
    reg [3:0]   p_r0;         // first row within strip
    reg [3:0]   p_nr;         // rows - 1
    reg [11:0]  p_a0;         // bank address of the first group
    reg         p_zt, p_zw, p_ne;

    // ------------------------------------------------------------------ span state
    reg         run;
    reg [223:0] cur, nrow, dvx, dvx3, dvx5, dvx7, dvy;
    reg [7:0]   gleft;       // groups left in the row after the current one
    reg [7:0]   gspan;       // ge - gs
    reg [3:0]   rleft;       // rows left after the current one
    reg         firstg, lastg, single;
    reg [7:0]   mf, ml;
    reg [11:0]  addr, naddr;
    reg         zt, zw, ne;

    // ------------------------------------------------------------------ pipeline regs
    reg         s0_v;
    reg [223:0] s0_c;
    reg [11:0]  s0_addr;
    reg [7:0]   s0_m;
    reg         s1_v, s2_v, s3_v;
    reg [7:0]   s1_cov, s2_cov;
    reg [127:0] s1_zp, s2_zp, s1_col, s2_col;
    reg [11:0]  s1_addr, s2_addr, s3_addr;
    reg [7:0]   s3_cwe;
    reg         s3_zwe;
    reg [127:0] s3_zm, s3_col;

    wire pipe_empty = !s0_v && !s1_v && !s2_v && !s3_v;
    wire launch     = (sst == SS_READY) && !run && pipe_empty && go_ok;

    assign setup_busy = (sst != SS_IDLE);
    assign idle_all   = (sst == SS_IDLE) && !run && pipe_empty;

    function [7:0] first_mask;  // lanes j >= xo
        input [2:0] xo;
        begin
            first_mask = 8'hFF << xo;
        end
    endfunction
    function [7:0] last_mask;   // lanes j <= xl
        input [2:0] xl;
        begin
            last_mask = 8'hFF >> (3'd7 - xl);
        end
    endfunction

    integer v;

    // ------------------------------------------------------------------ setup
    always @(posedge clk) begin
        if (rst) begin
            sst <= SS_IDLE;
        end else begin
            case (sst)
                SS_IDLE: if (start) sst <= SS_MUL;
                SS_MUL:  if (mb == 10'd0) sst <= SS_XOFF;
                SS_XOFF: sst <= SS_READY;
                default: if (launch) sst <= SS_IDLE;
            endcase
        end
    end

    always @(posedge clk) begin
        if (sst == SS_IDLE && start) begin
            for (v = 0; v < 7; v = v + 1) begin
                acc[v*32 +: 32]  <= rv0[v*32 +: 32];
                mm[v*32 +: 32]   <= rdy[v*32 +: 32];
                sdx[v*32 +: 32]  <= rdx[v*32 +: 32];
                sdx3[v*32 +: 32] <= rdx[v*32 +: 32] + {rdx[v*32 +: 31], 1'b0};
                sdx5[v*32 +: 32] <= rdx[v*32 +: 32] + {rdx[v*32 +: 30], 2'b00};
                sdx7[v*32 +: 32] <= {rdx[v*32 +: 29], 3'b000} - rdx[v*32 +: 32];
                sdy[v*32 +: 32]  <= rdy[v*32 +: 32];
            end
            mb    <= dy0[9:0];
            p_xo  <= xmin[2:0];
            p_gs  <= xmin[10:3];
            p_ge  <= xe[10:3];
            p_mf  <= first_mask(xmin[2:0]);
            p_ml  <= last_mask(xe[2:0]);
            p_r0  <= ys[3:0];
            p_nr  <= ye[3:0] - ys[3:0];
            p_zt  <= w0[24];
            p_zw  <= w0[25];
            p_ne  <= w0[27];
        end else if (sst == SS_MUL) begin
            if (mb != 10'd0) begin
                for (v = 0; v < 7; v = v + 1) begin
                    if (mb[0])
                        acc[v*32 +: 32] <= acc[v*32 +: 32] + mm[v*32 +: 32];
                    mm[v*32 +: 32] <= {mm[v*32 +: 31], 1'b0};
                end
                mb <= {1'b0, mb[9:1]};
            end
            // r0*160 + gs
            p_a0 <= {1'b0, p_r0, 7'd0} + {3'b000, p_r0, 5'd0} + {4'd0, p_gs};
        end else if (sst == SS_XOFF) begin
            for (v = 0; v < 7; v = v + 1) begin
                case (p_xo)
                    3'd0: acc[v*32 +: 32] <= acc[v*32 +: 32];
                    3'd1: acc[v*32 +: 32] <= acc[v*32 +: 32] - sdx[v*32 +: 32];
                    3'd2: acc[v*32 +: 32] <= acc[v*32 +: 32] - {sdx[v*32 +: 31], 1'b0};
                    3'd3: acc[v*32 +: 32] <= acc[v*32 +: 32] - sdx3[v*32 +: 32];
                    3'd4: acc[v*32 +: 32] <= acc[v*32 +: 32] - {sdx[v*32 +: 30], 2'b00};
                    3'd5: acc[v*32 +: 32] <= acc[v*32 +: 32] - sdx5[v*32 +: 32];
                    3'd6: acc[v*32 +: 32] <= acc[v*32 +: 32] - {sdx3[v*32 +: 31], 1'b0};
                    default: acc[v*32 +: 32] <= acc[v*32 +: 32] - sdx7[v*32 +: 32];
                endcase
            end
        end
    end

    // ------------------------------------------------------------------ span iterator
    always @(posedge clk) begin
        if (rst) begin
            run  <= 1'b0;
            s0_v <= 1'b0;
        end else begin
            if (launch) begin
                run  <= 1'b1;
                s0_v <= 1'b0;
            end else if (run) begin
                s0_v <= 1'b1;
                if (lastg && (rleft == 4'd0))
                    run <= 1'b0;
            end else begin
                s0_v <= 1'b0;
            end
        end
    end

    always @(posedge clk) begin
        if (launch) begin
            for (v = 0; v < 7; v = v + 1)
                nrow[v*32 +: 32] <= acc[v*32 +: 32] + sdy[v*32 +: 32];
            cur    <= acc;
            dvx    <= sdx;
            dvx3   <= sdx3;
            dvx5   <= sdx5;
            dvx7   <= sdx7;
            dvy    <= sdy;
            gspan  <= p_ge - p_gs;
            gleft  <= p_ge - p_gs;
            rleft  <= p_nr;
            firstg <= 1'b1;
            lastg  <= (p_ge == p_gs);
            single <= (p_ge == p_gs);
            mf     <= p_mf;
            ml     <= p_ml;
            addr   <= p_a0;
            naddr  <= p_a0 + 12'd160;
            zt     <= p_zt;
            zw     <= p_zw;
            ne     <= p_ne;
        end else if (run) begin
            // advance
            if (lastg) begin
                for (v = 0; v < 7; v = v + 1)
                    nrow[v*32 +: 32] <= nrow[v*32 +: 32] + dvy[v*32 +: 32];
                cur    <= nrow;
                addr   <= naddr;
                naddr  <= naddr + 12'd160;
                gleft  <= gspan;
                rleft  <= rleft - 4'd1;
                firstg <= 1'b1;
                lastg  <= single;
            end else begin
                for (v = 0; v < 7; v = v + 1)
                    cur[v*32 +: 32] <= cur[v*32 +: 32] + {dvx[v*32 +: 29], 3'b000};
                addr   <= addr + 12'd1;
                gleft  <= gleft - 8'd1;
                firstg <= 1'b0;
                lastg  <= (gleft == 8'd1);
            end
        end
    end

    // S0 payload: loaded every cycle (no clock enable: `run` would fan out to ~250 flip-flop
    // enables); it only matters while s0_v = 1, i.e. in the cycle after a `run` cycle.
    always @(posedge clk) begin
        s0_c    <= cur;
        s0_addr <= addr;
        s0_m    <= (firstg ? mf : 8'hFF) & (lastg ? ml : 8'hFF);
    end

    // ------------------------------------------------------------------ S1: lanes + clamps
    /* verilator lint_off UNUSED */
    function [15:0] zclamp;
        input [31:0] z;
        begin
            if (z[31])
                zclamp = 16'h0000;
            else if (z[30:28] != 3'b000)
                zclamp = 16'hFFFF;
            else
                zclamp = z[27:12];
        end
    endfunction
    /* verilator lint_on UNUSED */
    /* verilator lint_off UNUSED */
    function [7:0] cclamp;
        input [31:0] c;
        begin
            if (c[31])
                cclamp = 8'h00;
            else if (c[30:24] != 7'd0)
                cclamp = 8'hFF;
            else
                cclamp = c[23:16];
        end
    endfunction
    /* verilator lint_on UNUSED */

    wire [7:0]   cov_c;
    wire [127:0] zp_c, col_c;
    wire [7:0]   pass_c;
    wire [127:0] zm_c;
    /* verilator lint_off UNUSED */
    wire [9:0]   z_spare = z_rdata[143:134];
    /* verilator lint_on UNUSED */
    // S2: effective old Z (stale tag -> cleared)
    // strip_q: local copy of the strip index for the Z tags (strip only changes between strips,
    // with the pipeline empty and >= 5 cycles before the next primitive reaches S2)
    reg  [5:0]   strip_q;
    always @(posedge clk)
        strip_q <= strip;
    wire         ztag_ok = (z_rdata[133:128] == strip_q);
    wire [127:0] zold    = ztag_ok ? z_rdata[127:0] : {128{1'b1}};

    genvar gj, gv;
    generate
        for (gj = 0; gj < 8; gj = gj + 1) begin : g_lane
            /* verilator lint_off UNUSED */
            wire [223:0] lv;
            /* verilator lint_on UNUSED */
            for (gv = 0; gv < 7; gv = gv + 1) begin : g_val
                wire [31:0] c  = s0_c[gv*32 +: 32];
                wire [31:0] d  = dvx[gv*32 +: 32];
                wire [31:0] d3 = dvx3[gv*32 +: 32];
                wire [31:0] d5 = dvx5[gv*32 +: 32];
                wire [31:0] d7 = dvx7[gv*32 +: 32];
                wire [31:0] off = (gj == 0) ? 32'd0 :
                                  (gj == 1) ? d :
                                  (gj == 2) ? {d[30:0], 1'b0} :
                                  (gj == 3) ? d3 :
                                  (gj == 4) ? {d[29:0], 2'b00} :
                                  (gj == 5) ? d5 :
                                  (gj == 6) ? {d3[30:0], 1'b0} : d7;
                assign lv[gv*32 +: 32] = c + off;
            end
            /* verilator lint_off UNUSED */
            wire [7:0] r8 = cclamp(lv[4*32 +: 32]);
            wire [7:0] g8 = cclamp(lv[5*32 +: 32]);
            wire [7:0] b8 = cclamp(lv[6*32 +: 32]);
            /* verilator lint_on UNUSED */
            assign cov_c[gj] = s0_m[gj] && (ne || (!lv[0*32+31] && !lv[1*32+31] && !lv[2*32+31]));
            assign zp_c[gj*16 +: 16]  = zclamp(lv[3*32 +: 32]);
            assign col_c[gj*16 +: 16] = {r8[7:3], g8[7:2], b8[7:3]};
            // a stale word reads as 0xFFFF and every zp <= 0xFFFF: the 16-bit compare runs on the
            // raw data in parallel with the tag compare
            assign pass_c[gj] = s2_cov[gj] &&
                                (!zt || !ztag_ok || (s2_zp[gj*16 +: 16] <= z_rdata[gj*16 +: 16]));
            assign zm_c[gj*16 +: 16] = pass_c[gj] ? s2_zp[gj*16 +: 16] : zold[gj*16 +: 16];
        end
    endgenerate

    // ------------------------------------------------------------------ S1..S3
    always @(posedge clk) begin
        if (rst) begin
            s1_v   <= 1'b0;
            s2_v   <= 1'b0;
            s3_v   <= 1'b0;
            s3_cwe <= 8'h00;
            s3_zwe <= 1'b0;
        end else begin
            s1_v   <= s0_v;
            s2_v   <= s1_v;
            s3_v   <= s2_v;
            s3_cwe <= s2_v ? pass_c : 8'h00;
            s3_zwe <= s2_v && zw;
        end
    end

    always @(posedge clk) begin
        s1_cov  <= cov_c;
        s1_zp   <= zp_c;
        s1_col  <= col_c;
        s1_addr <= s0_addr;
        s2_cov  <= s1_cov;
        s2_zp   <= s1_zp;
        s2_col  <= s1_col;
        s2_addr <= s1_addr;
        s3_zm   <= zm_c;
        s3_col  <= s2_col;
        s3_addr <= s2_addr;
    end

    assign z_re     = s0_v;
    assign z_raddr  = s0_addr;
    assign c_we     = s3_cwe;
    assign z_we     = s3_zwe;
    assign w_addr   = s3_addr;
    assign c_wdata  = s3_col;
    assign z_wdata  = {10'd0, strip_q, s3_zm};
    assign w_active = s3_v;
endmodule
