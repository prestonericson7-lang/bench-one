// pz7020_ps7_top.v -- PS + PL together on the PZ7020-StarLite, built with the OPEN toolchain.
//
// The Zynq PS7 hard block is instantiated as the `PS7` primitive (yosys knows it; nextpnr-xilinx
// places it on the one PS7 site). Only the ports this design uses are connected -- the PS side
// (DDR, MIO, clocks) is bonded internally and configured by U-Boot SPL's ps7_init, not by the PL.
//
//   FCLKCLK[0]      100 MHz from the PS PLL (PCW_FPGA0_PERIPHERAL_FREQMHZ) -> BUFG -> everything here
//   FCLKRESETN[0]   PS-controlled PL reset (released by ps7_post_config / the FSBL-equivalent)
//   M_AXI_GP0       AXI3 master at 0x4000_0000 -> adapted to AXI4-Lite -> pl_regs
//
// Linux reads/writes pl_regs through /dev/mem at 0x40000000 (firmware/telemetry-hub/pl_regs.py).
// Port wiring follows the openXC7 ps7_axi_blinky demo (MIT), which runs on the same xc7z020clg400.
`default_nettype none
`timescale 1ns / 1ps

module pz7020_ps7_top (
    output wire led1,       // R19 heartbeat (register-enabled)
    output wire led2,       // V13 software bit
    input  wire key1_n,     // G14
    input  wire key2_n,     // J15
    output wire fan_pwm,    // JM1 pin 5  H16
    input  wire fan_tach    // JM1 pin 7  H17
);
    // ---------------- clocks / reset from the PS ----------------
    wire [3:0] fclk_raw, fclk_rstn;
    wire       aclk;
    BUFG bufg_fclk0 (.I(fclk_raw[0]), .O(aclk));
    wire aresetn = fclk_rstn[0];

    // ---------------- M_AXI_GP0 (AXI3 from the PS) ----------------
    wire        gp0_arvalid, gp0_awvalid, gp0_bready, gp0_rready, gp0_wlast, gp0_wvalid, gp0_aresetn;
    wire [11:0] gp0_arid, gp0_awid, gp0_wid;
    wire [31:0] gp0_araddr, gp0_awaddr, gp0_wdata;
    wire [3:0]  gp0_wstrb;
    wire        gp0_arready, gp0_awready, gp0_bvalid, gp0_rvalid, gp0_wready;
    wire [1:0]  gp0_bresp, gp0_rresp;
    wire [31:0] gp0_rdata;

    // AXI3 -> AXI4-Lite: single-beat transfers; echo the ID of the accepted address on the response
    reg [11:0] bid_q = 12'd0, rid_q = 12'd0;
    always @(posedge aclk) begin
        if (gp0_awvalid && gp0_awready) bid_q <= gp0_awid;
        if (gp0_arvalid && gp0_arready) rid_q <= gp0_arid;
    end

    pl_regs #(.CLK_HZ(100_000_000)) regs (
        .aclk(aclk), .aresetn(aresetn & gp0_aresetn),
        .s_axi_awaddr(gp0_awaddr), .s_axi_awvalid(gp0_awvalid), .s_axi_awready(gp0_awready),
        .s_axi_wdata(gp0_wdata), .s_axi_wstrb(gp0_wstrb), .s_axi_wvalid(gp0_wvalid), .s_axi_wready(gp0_wready),
        .s_axi_bresp(gp0_bresp), .s_axi_bvalid(gp0_bvalid), .s_axi_bready(gp0_bready),
        .s_axi_araddr(gp0_araddr), .s_axi_arvalid(gp0_arvalid), .s_axi_arready(gp0_arready),
        .s_axi_rdata(gp0_rdata), .s_axi_rresp(gp0_rresp), .s_axi_rvalid(gp0_rvalid), .s_axi_rready(gp0_rready),
        .led1(led1), .led2(led2), .key1_n(key1_n), .key2_n(key2_n), .fan_pwm(fan_pwm), .fan_tach(fan_tach));

    // ---------------- the hard PS ----------------
    PS7 ps7 (
        .FCLKCLK        (fclk_raw),
        .FCLKRESETN     (fclk_rstn),
        .FCLKCLKTRIGN   (4'b1111),
        // GP0 outputs (PS -> PL)
        .MAXIGP0ACLK    (aclk),
        .MAXIGP0ARESETN (gp0_aresetn),
        .MAXIGP0ARADDR  (gp0_araddr),  .MAXIGP0ARVALID (gp0_arvalid), .MAXIGP0ARID (gp0_arid),
        .MAXIGP0AWADDR  (gp0_awaddr),  .MAXIGP0AWVALID (gp0_awvalid), .MAXIGP0AWID (gp0_awid),
        .MAXIGP0WDATA   (gp0_wdata),   .MAXIGP0WVALID  (gp0_wvalid),  .MAXIGP0WSTRB (gp0_wstrb),
        .MAXIGP0WLAST   (gp0_wlast),   .MAXIGP0WID     (gp0_wid),
        .MAXIGP0BREADY  (gp0_bready),  .MAXIGP0RREADY  (gp0_rready),
        // GP0 inputs (PL -> PS)
        .MAXIGP0ARREADY (gp0_arready), .MAXIGP0AWREADY (gp0_awready), .MAXIGP0WREADY (gp0_wready),
        .MAXIGP0BVALID  (gp0_bvalid),  .MAXIGP0BRESP   (gp0_bresp),   .MAXIGP0BID    (bid_q),
        .MAXIGP0RVALID  (gp0_rvalid),  .MAXIGP0RRESP   (gp0_rresp),   .MAXIGP0RDATA  (gp0_rdata),
        .MAXIGP0RID     (rid_q),       .MAXIGP0RLAST   (1'b1),
        // nothing else: GP1, HP, ACP, EMIO, DMA, IRQ all unused
        .IRQF2P         (20'd0),
        .DDRARB         (4'd0)
    );
endmodule
`default_nettype wire
