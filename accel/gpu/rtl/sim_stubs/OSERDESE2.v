// OSERDESE2.v -- SIMULATION-ONLY behavioural stub of the Xilinx 7-series OSERDESE2 primitive
// (parameter and port names as in the UNISIM library).  Never synthesise this file: Vivado
// uses the real primitive.
//
// Modelled: DATA_RATE_OQ = "DDR", DATA_WIDTH = 10, master/slave width expansion as used by
// rtl/video_serializer.v (the only supported configuration; anything else prints an ERROR).
//  * SLAVE: SHIFTOUT1 = D3, SHIFTOUT2 = D4 (combinational stand-in for the real shift chain;
//    the slave's D inputs come from the same CLKDIV register as the master's).
//  * MASTER: at every CLKDIV rising edge captures word = {SHIFTIN2, SHIFTIN1, D8..D1}
//    (SRVAL_OQ bits while RST is high or X).  The word is loaded into the output shift
//    register at the first CLK edge strictly after that CLKDIV edge and shifted out one bit
//    per CLK edge (both edges, DDR), D1 first: OQ sequence = word[0], word[1], ..., word[9].
//  * Checks that exactly DATA_WIDTH CLK edges occur per CLKDIV period (CLK must be 5x CLKDIV,
//    phase aligned); violations are printed and counted in stub_errors.
//  Latency differs from silicon (irrelevant for DVI: all lanes share it).
// T/TQ/TFB/TBYTEOUT are tied to 0, OFB follows OQ.
/* verilator lint_off WIDTH */
/* verilator lint_off UNUSED */
/* verilator lint_off COMBDLY */
/* verilator lint_off UNOPTFLAT */
/* verilator lint_off BLKSEQ */
module OSERDESE2 #(
    parameter DATA_RATE_OQ   = "DDR",
    parameter DATA_RATE_TQ   = "DDR",
    parameter integer DATA_WIDTH = 4,
    parameter [0:0] INIT_OQ  = 1'b0,
    parameter [0:0] INIT_TQ  = 1'b0,
    parameter [0:0] IS_CLKDIV_INVERTED = 1'b0,
    parameter [0:0] IS_CLK_INVERTED    = 1'b0,
    parameter [0:0] IS_D1_INVERTED = 1'b0,
    parameter [0:0] IS_D2_INVERTED = 1'b0,
    parameter [0:0] IS_D3_INVERTED = 1'b0,
    parameter [0:0] IS_D4_INVERTED = 1'b0,
    parameter [0:0] IS_D5_INVERTED = 1'b0,
    parameter [0:0] IS_D6_INVERTED = 1'b0,
    parameter [0:0] IS_D7_INVERTED = 1'b0,
    parameter [0:0] IS_D8_INVERTED = 1'b0,
    parameter [0:0] IS_T1_INVERTED = 1'b0,
    parameter [0:0] IS_T2_INVERTED = 1'b0,
    parameter [0:0] IS_T3_INVERTED = 1'b0,
    parameter [0:0] IS_T4_INVERTED = 1'b0,
    parameter SERDES_MODE    = "MASTER",
    parameter [0:0] SRVAL_OQ = 1'b0,
    parameter [0:0] SRVAL_TQ = 1'b0,
    parameter TBYTE_CTL      = "FALSE",
    parameter TBYTE_SRC      = "FALSE",
    parameter integer TRISTATE_WIDTH = 4
) (
    output wire OFB,
    output wire OQ,
    output wire SHIFTOUT1,
    output wire SHIFTOUT2,
    output wire TBYTEOUT,
    output wire TFB,
    output wire TQ,
    input  wire CLK,
    input  wire CLKDIV,
    input  wire D1,
    input  wire D2,
    input  wire D3,
    input  wire D4,
    input  wire D5,
    input  wire D6,
    input  wire D7,
    input  wire D8,
    input  wire OCE,
    input  wire RST,
    input  wire SHIFTIN1,
    input  wire SHIFTIN2,
    input  wire T1,
    input  wire T2,
    input  wire T3,
    input  wire T4,
    input  wire TBYTEIN,
    input  wire TCE
);
    localparam IS_MASTER = (SERDES_MODE == "MASTER");

    integer stub_errors = 0;

    initial begin
        if (DATA_RATE_OQ != "DDR" || DATA_WIDTH != 10) begin
            $display("ERROR OSERDESE2 stub %m: only DATA_RATE_OQ=DDR, DATA_WIDTH=10 is modelled");
            stub_errors = stub_errors + 1;
        end
        if (SERDES_MODE != "MASTER" && SERDES_MODE != "SLAVE") begin
            $display("ERROR OSERDESE2 stub %m: bad SERDES_MODE");
            stub_errors = stub_errors + 1;
        end
        if (IS_CLK_INVERTED || IS_CLKDIV_INVERTED) begin
            $display("ERROR OSERDESE2 stub %m: clock inversion not modelled");
            stub_errors = stub_errors + 1;
        end
    end

    wire d1 = D1 ^ IS_D1_INVERTED;
    wire d2 = D2 ^ IS_D2_INVERTED;
    wire d3 = D3 ^ IS_D3_INVERTED;
    wire d4 = D4 ^ IS_D4_INVERTED;
    wire d5 = D5 ^ IS_D5_INVERTED;
    wire d6 = D6 ^ IS_D6_INVERTED;
    wire d7 = D7 ^ IS_D7_INVERTED;
    wire d8 = D8 ^ IS_D8_INVERTED;

    // ---- slave: pass its D3/D4 to the master ----
    assign SHIFTOUT1 = IS_MASTER ? 1'b0 : d3;
    assign SHIFTOUT2 = IS_MASTER ? 1'b0 : d4;

    // ---- master: parallel capture on CLKDIV, DDR shift-out on CLK ----
    reg [9:0] word = {10{INIT_OQ}};
    reg       tog  = 1'b0;
    reg       seen = 1'b0;
    reg [9:0] sh   = {10{INIT_OQ}};
    reg       oq_r = INIT_OQ;
    integer   nbits = 0;
    integer   loads = 0;
    realtime  t_div = 0.0;                      // time of the last CLKDIV rising edge

    always @(posedge CLKDIV) begin
        if (RST !== 1'b0)                       // X/Z on RST is treated as reset
            word <= {10{SRVAL_OQ}};
        else if (OCE)
            word <= {SHIFTIN2, SHIFTIN1, d8, d7, d6, d5, d4, d3, d2, d1};
        tog   <= ~tog;
        t_div  = $realtime;
    end

    // A new word is loaded at the first CLK edge strictly later than the CLKDIV edge (the
    // time check makes this independent of the delta-cycle order of coincident edges).
    always @(CLK) begin
        if (tog !== seen && $realtime > t_div) begin
            seen <= tog;
            if (IS_MASTER && loads >= 2 && nbits != DATA_WIDTH) begin
                stub_errors = stub_errors + 1;
                if (stub_errors <= 5)
                    $display("ERROR OSERDESE2 stub %m: %0d CLK edges per CLKDIV period (expected %0d) at %t",
                             nbits, DATA_WIDTH, $time);
            end
            loads <= loads + 1;
            nbits <= 1;
            oq_r  <= word[0];
            sh    <= {1'b0, word[9:1]};
        end else begin
            nbits <= nbits + 1;
            oq_r  <= sh[0];
            sh    <= {1'b0, sh[9:1]};
        end
    end

    assign OQ       = IS_MASTER ? oq_r : 1'b0;
    assign OFB      = OQ;
    assign TQ       = 1'b0;
    assign TFB      = 1'b0;
    assign TBYTEOUT = 1'b0;

    // unused inputs: T1..T4, TBYTEIN, TCE (tristate path not modelled)
    wire unused = T1 | T2 | T3 | T4 | TBYTEIN | TCE | IS_T1_INVERTED | IS_T2_INVERTED |
                  IS_T3_INVERTED | IS_T4_INVERTED | INIT_TQ | SRVAL_TQ;
endmodule
/* verilator lint_on BLKSEQ */
/* verilator lint_on UNOPTFLAT */
/* verilator lint_on COMBDLY */
/* verilator lint_on UNUSED */
/* verilator lint_on WIDTH */
