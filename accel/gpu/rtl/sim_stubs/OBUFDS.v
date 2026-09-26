// OBUFDS.v -- SIMULATION-ONLY stub of the Xilinx differential output buffer (UNISIM names).
// Never synthesise this file: Vivado uses the real primitive (IOSTANDARD set in the XDC).
module OBUFDS #(
    parameter CAPACITANCE = "DONT_CARE",
    parameter IOSTANDARD  = "DEFAULT",
    parameter SLEW        = "SLOW"
) (
    output wire O,
    output wire OB,
    input  wire I
);
    assign O  = I;
    assign OB = ~I;
endmodule
