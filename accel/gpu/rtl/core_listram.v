// core_listram.v -- record store for the two ping-pong lists: RING slots of 720 bits, organised
// as 10 column RAMs of RING x 72 (one block-RAM word per column and slot, parity bits used).
//
// Record packing (done by core_collector, undone by core_fetch): a stored record is TRI or
// SPRITE; word 23 is always 0 and words 0..2 are compressed, so 720 bits hold it exactly:
//   column c (c = 0..9) = { X[8c+7:8c], w(4+2c), w(3+2c) }            (72 bits)
//   X (80 bits) = { w2[31:0], w1[21:0], hdr[25:0] }
//   hdr = { is_sprite, w0[27], w0[25], w0[24], w0[21:0] }
// (w0[31:28] is 1 or 2 for stored records, w0[26] / w0[23:22] / w1[31:22] are unused by the PL).
//
// Write port: one column per cycle (the collector writes column c when w(4+2c) arrives).
// Read port: a whole slot (720 bits) at once.
// Latency: write and read inputs are registered once here (fan-out to 10 x RING/512 block RAMs),
// then core_ram (OREG = 1): raddr/re sampled at edge 1 -> rdata valid after edge 3.
// RING = 2048 -> 10 x 4 = 40 RAMB36 (2048 x 72 = 4 x 512x72 or 4 x 2Kx18 each).
module core_listram #(
    parameter RING = 2048,       // slots (power of two)
    parameter SAW  = 11          // log2(RING)
) (
    input  wire            clk,
    input  wire            we,
    input  wire [3:0]      wcol,        // 0..9
    input  wire [SAW-1:0]  waddr,
    input  wire [71:0]     wdata,
    input  wire            re,
    input  wire [SAW-1:0]  raddr,
    output wire [719:0]    rdata
);
    // each of these drives 40 block-RAM pins spread over the list RAM: let synthesis replicate
    (* max_fanout = 8 *) reg [9:0]     wsel_q;
    (* max_fanout = 8 *) reg [SAW-1:0] wa_q;
    (* max_fanout = 8 *) reg [71:0]    wd_q;
    (* max_fanout = 8 *) reg           re_q;
    (* max_fanout = 8 *) reg [SAW-1:0] ra_q;
    integer c;

    always @(posedge clk) begin
        for (c = 0; c < 10; c = c + 1)
            wsel_q[c] <= we && (wcol == c[3:0]);
        wa_q <= waddr;
        wd_q <= wdata;
        re_q <= re;
        ra_q <= raddr;
    end

    genvar g;
    generate
        for (g = 0; g < 10; g = g + 1) begin : g_col
            core_ram #(.DW(72), .NBE(1), .AW(SAW), .DEPTH(RING), .OREG(1)) u_col (
                .clk(clk), .we(wsel_q[g]), .waddr(wa_q), .wdata(wd_q),
                .re(re_q), .raddr(ra_q), .rdata(rdata[g*72 +: 72])
            );
        end
    endgenerate
endmodule
