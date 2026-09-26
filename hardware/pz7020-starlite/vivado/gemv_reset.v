// gemv_reset.v -- reset of the matrix engine (zaccel_gemv) in the system block design.
//
//   aresetn = peripheral_aresetn & mm2s_prmry_reset_out_n & s2mm_prmry_reset_out_n   (registered)
//
// The AXI DMA drives mm2s_prmry_reset_out_n / s2mm_prmry_reset_out_n low while its MM2S / S2MM
// channel is reset (hard reset, or a soft reset through DMACR bit 2). zaccel-server soft-resets both
// channels before every job, so the same pulse clears the engine: an aborted job cannot leave it
// half way through a header/activation/weight stream until the next reboot.
// All three inputs are registered outputs in the FCLK0 domain (proc_sys_reset, axi_dma), aclk is
// FCLK0; the AND is registered once so the engine's reset fan-out starts from a flop. Power-up
// value 0 = in reset.
`default_nettype none

module gemv_reset (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_RESET aresetn:peripheral_aresetn:mm2s_prmry_reset_out_n:s2mm_prmry_reset_out_n" *)
    input  wire aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 peripheral_aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire peripheral_aresetn,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 mm2s_prmry_reset_out_n RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire mm2s_prmry_reset_out_n,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 s2mm_prmry_reset_out_n RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire s2mm_prmry_reset_out_n,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    output wire aresetn
);
    reg r = 1'b0;
    always @(posedge aclk) r <= peripheral_aresetn & mm2s_prmry_reset_out_n & s2mm_prmry_reset_out_n;
    assign aresetn = r;
endmodule

`default_nettype wire
