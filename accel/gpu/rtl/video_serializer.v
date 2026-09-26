// video_serializer.v -- one TMDS lane: 10:1 DDR serialiser (OSERDESE2 master/slave cascade)
// driving an OBUFDS.
//
// clk_ser = 5 x clk_pix, phase aligned (same MMCM, both on BUFG).  d[9:0] must come from a
// clk_pix register; d[0] is transmitted first (OSERDESE2 sends D1 first): master D1..D8 =
// d[0..7], slave D3/D4 = d[8]/d[9], slave SHIFTOUT1/2 -> master SHIFTIN1/2 (UG471 width
// expansion).  rst must be synchronous to clk_pix (CLKDIV) and shared by all lanes so every
// lane leaves reset in the same CLKDIV cycle.
// The IOSTANDARD (TMDS_33) is set in the XDC, not here.
module video_serializer (
    input  wire       clk_pix,
    input  wire       clk_ser,
    input  wire       rst,
    input  wire [9:0] d,
    output wire       pad_p,
    output wire       pad_n
);
    wire shift1, shift2, oq;

    /* verilator lint_off PINCONNECTEMPTY */
    OSERDESE2 #(
        .DATA_RATE_OQ   ("DDR"),
        .DATA_RATE_TQ   ("SDR"),
        .DATA_WIDTH     (10),
        .INIT_OQ        (1'b0),
        .INIT_TQ        (1'b0),
        .SERDES_MODE    ("MASTER"),
        .SRVAL_OQ       (1'b0),
        .SRVAL_TQ       (1'b0),
        .TBYTE_CTL      ("FALSE"),
        .TBYTE_SRC      ("FALSE"),
        .TRISTATE_WIDTH (1)
    ) u_master (
        .OFB       (),
        .OQ        (oq),
        .SHIFTOUT1 (),
        .SHIFTOUT2 (),
        .TBYTEOUT  (),
        .TFB       (),
        .TQ        (),
        .CLK       (clk_ser),
        .CLKDIV    (clk_pix),
        .D1        (d[0]),
        .D2        (d[1]),
        .D3        (d[2]),
        .D4        (d[3]),
        .D5        (d[4]),
        .D6        (d[5]),
        .D7        (d[6]),
        .D8        (d[7]),
        .OCE       (1'b1),
        .RST       (rst),
        .SHIFTIN1  (shift1),
        .SHIFTIN2  (shift2),
        .T1        (1'b0),
        .T2        (1'b0),
        .T3        (1'b0),
        .T4        (1'b0),
        .TBYTEIN   (1'b0),
        .TCE       (1'b0)
    );

    OSERDESE2 #(
        .DATA_RATE_OQ   ("DDR"),
        .DATA_RATE_TQ   ("SDR"),
        .DATA_WIDTH     (10),
        .INIT_OQ        (1'b0),
        .INIT_TQ        (1'b0),
        .SERDES_MODE    ("SLAVE"),
        .SRVAL_OQ       (1'b0),
        .SRVAL_TQ       (1'b0),
        .TBYTE_CTL      ("FALSE"),
        .TBYTE_SRC      ("FALSE"),
        .TRISTATE_WIDTH (1)
    ) u_slave (
        .OFB       (),
        .OQ        (),
        .SHIFTOUT1 (shift1),
        .SHIFTOUT2 (shift2),
        .TBYTEOUT  (),
        .TFB       (),
        .TQ        (),
        .CLK       (clk_ser),
        .CLKDIV    (clk_pix),
        .D1        (1'b0),
        .D2        (1'b0),
        .D3        (d[8]),
        .D4        (d[9]),
        .D5        (1'b0),
        .D6        (1'b0),
        .D7        (1'b0),
        .D8        (1'b0),
        .OCE       (1'b1),
        .RST       (rst),
        .SHIFTIN1  (1'b0),
        .SHIFTIN2  (1'b0),
        .T1        (1'b0),
        .T2        (1'b0),
        .T3        (1'b0),
        .T4        (1'b0),
        .TBYTEIN   (1'b0),
        .TCE       (1'b0)
    );

    /* verilator lint_on PINCONNECTEMPTY */

    OBUFDS u_obuf (
        .I  (oq),
        .O  (pad_p),
        .OB (pad_n)
    );
endmodule
