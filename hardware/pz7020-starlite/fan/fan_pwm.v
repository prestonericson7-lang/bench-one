`timescale 1ns / 1ps
//-----------------------------------------------------------------------------
// fan_pwm.v -- 25 kHz PWM generator + tachometer for a 4-wire DC fan
//
// Target : Xilinx Zynq XC7Z020 (PZ7020-StarLite), PL fabric
// Clock  : 50 MHz single-ended input, package ball U18 (IO_12P_MRCC_34)
//
// A 4-wire fan's PWM input is a logic-level input to a MOSFET *inside the fan*,
// specified at 25 kHz (usable 21-28 kHz) and 3.3 V-compatible, so a Zynq
// LVCMOS33 output drives it directly with no external parts.
//
// HARD RULE: never source fan motor current from an FPGA pin. The fan's +5V/+12V
// comes from a supply rail; the FPGA provides only PWM (and reads TACH).
//
// The TACH line is an open-collector output from the fan -- enable a pull-up on
// the input pin (see constraints/fan_jm1.xdc).
//-----------------------------------------------------------------------------
module fan_pwm #(
    parameter integer CLK_HZ              = 50_000_000, // fabric clock
    parameter integer PWM_HZ              = 25_000,     // 4-wire fan spec
    parameter integer TACH_PULSES_PER_REV = 2           // standard for PC fans
)(
    input  wire        clk,
    input  wire        rst_n,      // active low
    input  wire [7:0]  duty_pct,   // 0..100 (values >100 clamp to full on)
    output reg         fan_pwm,    // -> fan PWM wire
    input  wire        fan_tach,   // <- fan TACH wire (needs pull-up)
    output reg [15:0]  rpm         // refreshed once per second
);

    // ------------------------------------------------------------------
    // PWM generator
    //
    // PERIOD = 50e6 / 25e3 = 2000 ticks. STEP = PERIOD/100 = 20 ticks per
    // percent, so the threshold is a small constant multiply rather than a
    // runtime divide. If PERIOD is not a multiple of 100 the duty resolution
    // is truncated slightly -- irrelevant for a fan.
    // ------------------------------------------------------------------
    localparam integer PERIOD = CLK_HZ / PWM_HZ;
    localparam integer STEP   = PERIOD / 100;
    localparam integer PW     = $clog2(PERIOD + 1);

    reg [PW-1:0] pwm_cnt;
    reg [PW-1:0] thresh;

    always @(posedge clk) begin
        if (!rst_n) begin
            thresh <= {PW{1'b0}};
        end else if (duty_pct >= 8'd100) begin
            thresh <= PERIOD[PW-1:0];              // 100% -> always high
        end else begin
            thresh <= STEP * duty_pct;
        end
    end

    always @(posedge clk) begin
        if (!rst_n) begin
            pwm_cnt <= {PW{1'b0}};
            fan_pwm <= 1'b0;
        end else begin
            pwm_cnt <= (pwm_cnt == PERIOD - 1) ? {PW{1'b0}} : pwm_cnt + 1'b1;
            fan_pwm <= (pwm_cnt < thresh);
        end
    end

    // ------------------------------------------------------------------
    // Tachometer
    //
    // Two-flop synchroniser on the asynchronous TACH input, rising-edge
    // detect, and a count over a one-second window.
    //   RPM = pulses_per_second * 60 / pulses_per_rev   (= x30 for 2 ppr)
    // ------------------------------------------------------------------
    localparam integer RPM_SCALE = 60 / TACH_PULSES_PER_REV;

    reg  [1:0]  tach_sync;
    reg         tach_prev;
    reg  [31:0] sec_cnt;
    reg  [15:0] edge_cnt;
    wire        tach_rise = tach_sync[1] & ~tach_prev;

    always @(posedge clk) begin
        if (!rst_n) begin
            tach_sync <= 2'b11;          // idle high (pulled up)
            tach_prev <= 1'b1;
            sec_cnt   <= 32'd0;
            edge_cnt  <= 16'd0;
            rpm       <= 16'd0;
        end else begin
            tach_sync <= {tach_sync[0], fan_tach};
            tach_prev <= tach_sync[1];

            if (sec_cnt == CLK_HZ - 1) begin
                sec_cnt  <= 32'd0;
                rpm      <= edge_cnt * RPM_SCALE;
                // carry an edge landing on the boundary into the next window
                edge_cnt <= tach_rise ? 16'd1 : 16'd0;
            end else begin
                sec_cnt  <= sec_cnt + 32'd1;
                if (tach_rise) edge_cnt <= edge_cnt + 16'd1;
            end
        end
    end

endmodule
