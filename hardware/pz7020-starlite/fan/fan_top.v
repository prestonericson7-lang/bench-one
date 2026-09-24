`timescale 1ns / 1ps
//-----------------------------------------------------------------------------
// fan_top.v -- standalone top level for a first PZ7020-StarLite bitstream
//
// Purpose: one small design that proves three things at once --
//   1. the Vivado flow works end to end (build -> JTAG -> running),
//   2. the 50 MHz fabric clock on ball U18 is live (LED heartbeat),
//   3. the fan PWM output on JM1 works (fan spins at the set duty).
//
// PL-only: does not configure or depend on the PS, so it runs with the boot
// jumper on JTAG and nothing in QSPI/SD.
//
// Pin mapping (see constraints/fan_jm1.xdc and docs/pinout.md):
//   clk_50m   U18   50 MHz fabric clock input
//   led_hb    R19   user LED1 -- ~1 Hz heartbeat
//   fan_pwm   H16   JM1 pin 5  -> fan PWM wire
//   fan_tach  H17   JM1 pin 7  <- fan TACH wire (pull-up enabled)
//   key_n     G14   user KEY1 -- hold to step the fan to 100%
//-----------------------------------------------------------------------------
module fan_top #(
    parameter integer CLK_HZ       = 50_000_000,
    parameter integer DUTY_DEFAULT = 60           // percent, fan idle speed
)(
    input  wire clk_50m,
    output wire led_hb,
    output wire fan_pwm,
    input  wire fan_tach,
    input  wire key_n          // active low
);

    // ------------------------------------------------------------------
    // Reset: hold reset for ~1 ms after configuration, then release.
    // ------------------------------------------------------------------
    localparam integer RST_TICKS = CLK_HZ / 1000;
    reg [$clog2(RST_TICKS+1)-1:0] rst_cnt = 0;
    reg                           rst_n   = 1'b0;

    always @(posedge clk_50m) begin
        if (rst_cnt == RST_TICKS) rst_n <= 1'b1;
        else                      rst_cnt <= rst_cnt + 1'b1;
    end

    // ------------------------------------------------------------------
    // Heartbeat: toggles the LED ~1 Hz. If this LED blinks, the fabric is
    // configured and the U18 clock is really running at the expected rate.
    // ------------------------------------------------------------------
    reg [31:0] hb_cnt = 0;
    reg        hb     = 0;

    always @(posedge clk_50m) begin
        if (hb_cnt == (CLK_HZ/2) - 1) begin
            hb_cnt <= 32'd0;
            hb     <= ~hb;
        end else begin
            hb_cnt <= hb_cnt + 32'd1;
        end
    end

    assign led_hb = hb;

    // ------------------------------------------------------------------
    // Fan: DUTY_DEFAULT normally, 100% while KEY1 is held (audible proof
    // that the PWM duty is actually controlling the fan).
    // ------------------------------------------------------------------
    wire [7:0]  duty = key_n ? DUTY_DEFAULT[7:0] : 8'd100;
    wire [15:0] rpm;

    fan_pwm #(
        .CLK_HZ              (CLK_HZ),
        .PWM_HZ              (25_000),
        .TACH_PULSES_PER_REV (2)
    ) u_fan (
        .clk      (clk_50m),
        .rst_n    (rst_n),
        .duty_pct (duty),
        .fan_pwm  (fan_pwm),
        .fan_tach (fan_tach),
        .rpm      (rpm)
    );

    // `rpm` is left unconnected at top level on purpose: read it with the
    // Vivado ILA (Integrated Logic Analyzer), or export it to an AXI-Lite
    // register once the PS side is in use. Keep it from being optimised away
    // when probing with an ILA by marking it as a debug net in Vivado.
    (* KEEP = "TRUE" *) wire [15:0] rpm_keep = rpm;

endmodule
