// ---------------------------------------------------------------------------------------------
// clkgen -- PL clocks and resets (SPEC section 2)
//
//   clk50 (U18) -> MMCME2_ADV: DIVCLK 1, MULT 14.875 -> VCO 743.75 MHz
//     CLKOUT0 / 5.0  = 148.75  MHz  clk      core (collector, raster, AXI, bus receiver)
//     CLKOUT1 / 2    = 371.875 MHz  clk_ser  TMDS OSERDES CLK (5x pixel, DDR)
//     CLKOUT2 / 10   =  74.375 MHz  clk_pix  video timing, TMDS encode, OSERDES CLKDIV
//   All outputs through BUFG; feedback CLKFBOUT -> BUFG -> CLKFBIN (COMPENSATION ZHOLD).
//   CLKOUT1 and CLKOUT2 come from the same VCO with phase 0, so clk_ser and clk_pix are
//   phase-aligned (required by OSERDESE2 CLK/CLKDIV). clk is also exactly 2 x clk_pix from the
//   same VCO, but the design treats core <-> pixel as asynchronous (async FIFO / 2-FF
//   synchronisers only; set_clock_groups in fpga/pz7020_gpu.xdc), so the core divide can change
//   without touching the CDC logic. Vivado 2026.1 accepts this MMCM setting (fpga/probe).
//
//   rst / rst_pix: synchronous active-high, per domain. LOCKED goes through a 2-FF synchroniser
//   in each domain; the reset is released 16 cycles after the synchronised LOCKED is seen high
//   and re-asserted immediately if LOCKED drops. Both reset registers power up asserted.
//   locked = raw MMCM LOCKED (asynchronous; synchronise before use).
// ---------------------------------------------------------------------------------------------
module clkgen (
    input  wire clk50,
    output wire clk,
    output wire clk_pix,
    output wire clk_ser,
    output wire locked,
    output wire rst,
    output wire rst_pix
);
    wire clkfb_out, clkfb_buf;
    wire clk0_raw, clk1_raw, clk2_raw;
    wire mmcm_locked;

    /* verilator lint_off PINCONNECTEMPTY */
    MMCME2_ADV #(
        .BANDWIDTH            ("OPTIMIZED"),
        .CLKFBOUT_MULT_F      (14.875),
        .CLKFBOUT_PHASE       (0.0),
        .CLKFBOUT_USE_FINE_PS ("FALSE"),
        .CLKIN1_PERIOD        (20.0),
        .CLKIN2_PERIOD        (0.0),
        .DIVCLK_DIVIDE        (1),
        .CLKOUT0_DIVIDE_F     (5.0),          // 743.75 / 5 = 148.75 MHz core (SPEC 2)
        .CLKOUT0_DUTY_CYCLE   (0.5),
        .CLKOUT0_PHASE        (0.0),
        .CLKOUT0_USE_FINE_PS  ("FALSE"),
        .CLKOUT1_DIVIDE       (2),
        .CLKOUT1_DUTY_CYCLE   (0.5),
        .CLKOUT1_PHASE        (0.0),
        .CLKOUT1_USE_FINE_PS  ("FALSE"),
        .CLKOUT2_DIVIDE       (10),
        .CLKOUT2_DUTY_CYCLE   (0.5),
        .CLKOUT2_PHASE        (0.0),
        .CLKOUT2_USE_FINE_PS  ("FALSE"),
        .CLKOUT4_CASCADE      ("FALSE"),
        .COMPENSATION         ("ZHOLD"),
        .REF_JITTER1          (0.010),
        .REF_JITTER2          (0.010),
        .STARTUP_WAIT         ("FALSE")
    ) u_mmcm (
        .CLKIN1       (clk50),
        .CLKIN2       (1'b0),
        .CLKINSEL     (1'b1),          // 1 = CLKIN1
        .CLKFBIN      (clkfb_buf),
        .CLKFBOUT     (clkfb_out),
        .CLKFBOUTB    (),
        .CLKOUT0      (clk0_raw),
        .CLKOUT0B     (),
        .CLKOUT1      (clk1_raw),
        .CLKOUT1B     (),
        .CLKOUT2      (clk2_raw),
        .CLKOUT2B     (),
        .CLKOUT3      (),
        .CLKOUT3B     (),
        .CLKOUT4      (),
        .CLKOUT5      (),
        .CLKOUT6      (),
        .LOCKED       (mmcm_locked),
        .RST          (1'b0),
        .PWRDWN       (1'b0),
        .DADDR        (7'd0),
        .DCLK         (1'b0),
        .DEN          (1'b0),
        .DI           (16'd0),
        .DWE          (1'b0),
        .DO           (),
        .DRDY         (),
        .PSCLK        (1'b0),
        .PSEN         (1'b0),
        .PSINCDEC     (1'b0),
        .PSDONE       (),
        .CLKINSTOPPED (),
        .CLKFBSTOPPED ()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    BUFG u_bufg_fb  (.I(clkfb_out), .O(clkfb_buf));
    BUFG u_bufg_clk (.I(clk0_raw),  .O(clk));
    BUFG u_bufg_ser (.I(clk1_raw),  .O(clk_ser));
    BUFG u_bufg_pix (.I(clk2_raw),  .O(clk_pix));

    assign locked = mmcm_locked;

    // ---- core-domain reset --------------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) reg lk_c1 = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg lk_c2 = 1'b0;
    reg [4:0] cnt_c = 5'd0;
    reg       rst_c = 1'b1;

    always @(posedge clk) begin
        lk_c1 <= mmcm_locked;
        lk_c2 <= lk_c1;
        if (!lk_c2) begin
            cnt_c <= 5'd0;
            rst_c <= 1'b1;
        end else if (cnt_c != 5'd16) begin
            cnt_c <= cnt_c + 5'd1;
            rst_c <= 1'b1;
        end else begin
            rst_c <= 1'b0;
        end
    end
    assign rst = rst_c;

    // ---- pixel-domain reset -------------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) reg lk_p1 = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg lk_p2 = 1'b0;
    reg [4:0] cnt_p = 5'd0;
    reg       rst_p = 1'b1;

    always @(posedge clk_pix) begin
        lk_p1 <= mmcm_locked;
        lk_p2 <= lk_p1;
        if (!lk_p2) begin
            cnt_p <= 5'd0;
            rst_p <= 1'b1;
        end else if (cnt_p != 5'd16) begin
            cnt_p <= cnt_p + 5'd1;
            rst_p <= 1'b1;
        end else begin
            rst_p <= 1'b0;
        end
    end
    assign rst_pix = rst_p;

endmodule
