// BUFG.v -- SIMULATION-ONLY stub of the Xilinx global clock buffer (UNISIM names).
// Never synthesise this file: Vivado uses the real primitive. Zero-delay pass-through, so a
// BUFG'd clock and every flop it drives see the same simulation event.
module BUFG (
    output wire O,
    input  wire I
);
    assign O = I;
endmodule
