// core_ram2.v -- simple dual-port RAM of exactly DEPTH words, built from at most two
// power-of-two-deep core_ram pieces: LO = 2**floor(log2(DEPTH)) words and HI = DEPTH - LO words
// (HI must be 0 or a power of two, e.g. 5120 = 4096 + 1024, 2560 = 2048 + 512).
//
// Why: Vivado maps an inferred RAM with a non-power-of-two depth onto the next power of two
// when it picks a deep/narrow primitive aspect (the first OOC synthesis mapped the 5120 x 64
// strip banks as 8192 x 64 = 16 RAMB36 each instead of 10). Power-of-two pieces cannot be
// rounded up, so the block-RAM count is exact: 4096 x W + 1024 x W.
//
// Same ports / semantics / latency as core_ram with OREG = 1: raddr/re sampled at edge 1,
// rdata valid after edge 2 (the piece select is delayed alongside and muxes the two outputs).
// Read enable gating per piece: only the addressed piece is read.
module core_ram2 #(
    parameter DW    = 64,
    parameter NBE   = 8,
    parameter AW    = 13,
    parameter DEPTH = 5120
) (
    input  wire            clk,
    input  wire [NBE-1:0]  we,
    input  wire [AW-1:0]   waddr,
    input  wire [DW-1:0]   wdata,
    input  wire            re,
    input  wire [AW-1:0]   raddr,
    output wire [DW-1:0]   rdata
);
    function integer flog2;             // floor(log2(n)), n >= 1
        input integer n;
        integer k;
        begin
            flog2 = 0;
            for (k = n; k > 1; k = k >> 1)
                flog2 = flog2 + 1;
        end
    endfunction

    localparam LAW = flog2(DEPTH);      // LO = 2**LAW
    localparam LO  = 1 << LAW;
    localparam HI  = DEPTH - LO;
    localparam HAW = (HI > 1) ? flog2(HI) : 1;

    generate
        if (HI == 0) begin : g_one
            core_ram #(.DW(DW), .NBE(NBE), .AW(AW), .DEPTH(DEPTH), .OREG(1)) u_ram (
                .clk(clk), .we(we), .waddr(waddr), .wdata(wdata),
                .re(re), .raddr(raddr), .rdata(rdata)
            );
        end else begin : g_two
            // HI must be a power of two and the address must have room for LO + HI
            if (((1 << HAW) != HI) || (AW <= LAW)) begin : g_bad_depth
                /* verilator lint_off DECLFILENAME */
                core_ram2_bad_depth_parameter u_error ();   // elaboration error on purpose
                /* verilator lint_on DECLFILENAME */
            end
            wire           w_hi = waddr[LAW];
            wire           r_hi = raddr[LAW];
            reg            r_hi1, r_hi2;
            wire [DW-1:0]  d_lo, d_hi;
            core_ram #(.DW(DW), .NBE(NBE), .AW(LAW), .DEPTH(LO), .OREG(1)) u_lo (
                .clk(clk), .we(w_hi ? {NBE{1'b0}} : we), .waddr(waddr[LAW-1:0]), .wdata(wdata),
                .re(re && !r_hi), .raddr(raddr[LAW-1:0]), .rdata(d_lo)
            );
            core_ram #(.DW(DW), .NBE(NBE), .AW(HAW), .DEPTH(HI), .OREG(1)) u_hi (
                .clk(clk), .we(w_hi ? we : {NBE{1'b0}}), .waddr(waddr[HAW-1:0]), .wdata(wdata),
                .re(re && r_hi), .raddr(raddr[HAW-1:0]), .rdata(d_hi)
            );
            always @(posedge clk) begin
                if (re)
                    r_hi1 <= r_hi;
                r_hi2 <= r_hi1;
            end
            assign rdata = r_hi2 ? d_hi : d_lo;
            // address bits between HAW and LAW are 0 for every valid HI address
            /* verilator lint_off UNUSED */
            wire unused_hi = ^{waddr, raddr};
            /* verilator lint_on UNUSED */
        end
    endgenerate
endmodule
