`ifndef VERILATOR
`timescale 1ns / 1ps   // delays in this stub are in ns (iverilog only)
`endif
// MMCME2_ADV.v -- SIMULATION-ONLY behavioural stub of the Xilinx 7-series MMCME2_ADV primitive
// (parameter and port names as in the UNISIM library). Never synthesise this file: Vivado uses
// the real primitive.
//
// Modelled (iverilog):
//  * The VCO period is taken from the parameters, not measured:
//      T_vco = CLKIN1_PERIOD * DIVCLK_DIVIDE / CLKFBOUT_MULT_F
//    Every output is derived from ONE half-VCO tick generator by counting ticks, so all outputs
//    are exactly related (e.g. CLKOUT1 = 5 x CLKOUT2 and phase-aligned, as in silicon). The tick
//    is rounded to the 1 ps time precision (<0.1 % frequency error for the GPU configuration).
//  * CLKOUTn period = 2*DIVIDE ticks (DIVIDE may be a multiple of 0.5), 50 % duty (odd tick
//    counts: high one tick shorter). CLKOUTnB = ~CLKOUTn. Phases, duty-cycle and fine phase
//    shift parameters are ignored (a non-zero PHASE prints a warning).
//  * LOCKED rises LOCK_EDGES CLKIN1 rising edges after RST/PWRDWN are released (and CLKIN1 is
//    running); outputs are held low while unlocked. RST or PWRDWN high -> outputs low, unlocked.
//  * CLKFBOUT follows CLKIN1 (feedback is not modelled; CLKFBIN is ignored).
//  * DRP / phase shift: DO = 0, DRDY and PSDONE are single-cycle echoes of DEN / PSEN.
// Under Verilator (lint only) the outputs are simple pass-throughs of CLKIN1.
/* verilator lint_off UNUSED */
/* verilator lint_off UNDRIVEN */
module MMCME2_ADV #(
    parameter BANDWIDTH             = "OPTIMIZED",
    parameter real CLKFBOUT_MULT_F  = 5.000,
    parameter real CLKFBOUT_PHASE   = 0.000,
    parameter CLKFBOUT_USE_FINE_PS  = "FALSE",
    parameter real CLKIN1_PERIOD    = 0.000,
    parameter real CLKIN2_PERIOD    = 0.000,
    parameter real CLKOUT0_DIVIDE_F = 1.000,
    parameter real CLKOUT0_DUTY_CYCLE = 0.500,
    parameter real CLKOUT0_PHASE    = 0.000,
    parameter CLKOUT0_USE_FINE_PS   = "FALSE",
    parameter integer CLKOUT1_DIVIDE = 1,
    parameter real CLKOUT1_DUTY_CYCLE = 0.500,
    parameter real CLKOUT1_PHASE    = 0.000,
    parameter CLKOUT1_USE_FINE_PS   = "FALSE",
    parameter integer CLKOUT2_DIVIDE = 1,
    parameter real CLKOUT2_DUTY_CYCLE = 0.500,
    parameter real CLKOUT2_PHASE    = 0.000,
    parameter CLKOUT2_USE_FINE_PS   = "FALSE",
    parameter integer CLKOUT3_DIVIDE = 1,
    parameter real CLKOUT3_DUTY_CYCLE = 0.500,
    parameter real CLKOUT3_PHASE    = 0.000,
    parameter CLKOUT3_USE_FINE_PS   = "FALSE",
    parameter CLKOUT4_CASCADE       = "FALSE",
    parameter integer CLKOUT4_DIVIDE = 1,
    parameter real CLKOUT4_DUTY_CYCLE = 0.500,
    parameter real CLKOUT4_PHASE    = 0.000,
    parameter CLKOUT4_USE_FINE_PS   = "FALSE",
    parameter integer CLKOUT5_DIVIDE = 1,
    parameter real CLKOUT5_DUTY_CYCLE = 0.500,
    parameter real CLKOUT5_PHASE    = 0.000,
    parameter CLKOUT5_USE_FINE_PS   = "FALSE",
    parameter integer CLKOUT6_DIVIDE = 1,
    parameter real CLKOUT6_DUTY_CYCLE = 0.500,
    parameter real CLKOUT6_PHASE    = 0.000,
    parameter CLKOUT6_USE_FINE_PS   = "FALSE",
    parameter COMPENSATION          = "ZHOLD",
    parameter integer DIVCLK_DIVIDE = 1,
    parameter [0:0] IS_CLKINSEL_INVERTED = 1'b0,
    parameter [0:0] IS_PSEN_INVERTED     = 1'b0,
    parameter [0:0] IS_PSINCDEC_INVERTED = 1'b0,
    parameter [0:0] IS_PWRDWN_INVERTED   = 1'b0,
    parameter [0:0] IS_RST_INVERTED      = 1'b0,
    parameter real REF_JITTER1      = 0.010,
    parameter real REF_JITTER2      = 0.010,
    parameter SS_EN                 = "FALSE",
    parameter SS_MODE               = "CENTER_HIGH",
    parameter integer SS_MOD_PERIOD = 10000,
    parameter STARTUP_WAIT          = "FALSE"
) (
    output wire        CLKFBOUT,
    output wire        CLKFBOUTB,
    output wire        CLKFBSTOPPED,
    output wire        CLKINSTOPPED,
    output wire        CLKOUT0,
    output wire        CLKOUT0B,
    output wire        CLKOUT1,
    output wire        CLKOUT1B,
    output wire        CLKOUT2,
    output wire        CLKOUT2B,
    output wire        CLKOUT3,
    output wire        CLKOUT3B,
    output wire        CLKOUT4,
    output wire        CLKOUT5,
    output wire        CLKOUT6,
    output wire [15:0] DO,
    output wire        DRDY,
    output wire        LOCKED,
    output wire        PSDONE,
    input  wire        CLKFBIN,
    input  wire        CLKIN1,
    input  wire        CLKIN2,
    input  wire        CLKINSEL,
    input  wire [6:0]  DADDR,
    input  wire        DCLK,
    input  wire        DEN,
    input  wire [15:0] DI,
    input  wire        DWE,
    input  wire        PSCLK,
    input  wire        PSEN,
    input  wire        PSINCDEC,
    input  wire        PWRDWN,
    input  wire        RST
);
`ifdef VERILATOR
    assign CLKFBOUT = CLKIN1;   assign CLKFBOUTB = ~CLKIN1;
    assign CLKOUT0  = CLKIN1;   assign CLKOUT0B  = ~CLKIN1;
    assign CLKOUT1  = CLKIN1;   assign CLKOUT1B  = ~CLKIN1;
    assign CLKOUT2  = CLKIN1;   assign CLKOUT2B  = ~CLKIN1;
    assign CLKOUT3  = CLKIN1;   assign CLKOUT3B  = ~CLKIN1;
    assign CLKOUT4  = CLKIN1;   assign CLKOUT5   = CLKIN1;   assign CLKOUT6 = CLKIN1;
    assign LOCKED   = ~RST;
    assign DO = 16'd0; assign DRDY = 1'b0; assign PSDONE = 1'b0;
    assign CLKFBSTOPPED = 1'b0; assign CLKINSTOPPED = 1'b0;
`else
    localparam integer LOCK_EDGES = 16;

    reg [6:0] o = 7'd0;           // CLKOUT0..6
    reg       locked_r = 1'b0;
    reg       drdy_r = 1'b0, psdone_r = 1'b0;
    integer   lock_cnt = 0;
    integer   n [0:6];            // ticks per output period
    integer   h [0:6];            // ticks high
    integer   c [0:6];            // tick counters
    integer   k;
    real      thalf;              // half VCO period in ns

    wire rst_i = (RST ^ IS_RST_INVERTED) | (PWRDWN ^ IS_PWRDWN_INVERTED);

    initial begin
        n[0] = 2.0 * CLKOUT0_DIVIDE_F;  // real -> integer assignment rounds
        n[1] = 2 * CLKOUT1_DIVIDE;
        n[2] = 2 * CLKOUT2_DIVIDE;
        n[3] = 2 * CLKOUT3_DIVIDE;
        n[4] = 2 * CLKOUT4_DIVIDE;
        n[5] = 2 * CLKOUT5_DIVIDE;
        n[6] = 2 * CLKOUT6_DIVIDE;
        for (k = 0; k < 7; k = k + 1) begin
            if (n[k] < 2) n[k] = 2;
            h[k] = n[k] / 2;
            c[k] = 0;
        end
        if (CLKIN1_PERIOD <= 0.0) begin
            $display("MMCME2_ADV stub %m: ERROR CLKIN1_PERIOD must be set");
            thalf = 1.0;
        end else begin
            thalf = CLKIN1_PERIOD * DIVCLK_DIVIDE / CLKFBOUT_MULT_F / 2.0;
        end
        if (CLKOUT0_PHASE != 0.0 || CLKOUT1_PHASE != 0.0 || CLKOUT2_PHASE != 0.0 ||
            CLKOUT3_PHASE != 0.0 || CLKFBOUT_PHASE != 0.0)
            $display("MMCME2_ADV stub %m: WARNING non-zero PHASE parameters are ignored");
        $display("MMCME2_ADV stub %m: VCO %0.3f MHz, CLKOUT0 %0.3f / CLKOUT1 %0.3f / CLKOUT2 %0.3f MHz",
                 1000.0 / (2.0 * thalf), 1000.0 / (thalf * n[0]),
                 1000.0 / (thalf * n[1]), 1000.0 / (thalf * n[2]));
    end

    // lock detection on CLKIN1
    always @(posedge CLKIN1 or posedge rst_i) begin
        if (rst_i) begin
            lock_cnt <= 0;
            locked_r <= 1'b0;
        end else if (lock_cnt < LOCK_EDGES) begin
            lock_cnt <= lock_cnt + 1;
        end else begin
            locked_r <= 1'b1;
        end
    end

    // single tick generator for all outputs
    initial begin
        #0.001;
        forever begin
            #(thalf);
            if (locked_r) begin
                for (k = 0; k < 7; k = k + 1) begin
                    c[k] = (c[k] + 1 >= n[k]) ? 0 : c[k] + 1;
                    o[k] = (c[k] < h[k]);
                end
            end else begin
                for (k = 0; k < 7; k = k + 1) begin
                    c[k] = n[k] - 1;      // next tick -> 0 -> rising edge on all outputs together
                    o[k] = 1'b0;
                end
            end
        end
    end

    always @(posedge DCLK)  drdy_r   <= DEN;
    always @(posedge PSCLK) psdone_r <= PSEN;

    assign CLKOUT0 = o[0];  assign CLKOUT0B = ~o[0];
    assign CLKOUT1 = o[1];  assign CLKOUT1B = ~o[1];
    assign CLKOUT2 = o[2];  assign CLKOUT2B = ~o[2];
    assign CLKOUT3 = o[3];  assign CLKOUT3B = ~o[3];
    assign CLKOUT4 = o[4];
    assign CLKOUT5 = o[5];
    assign CLKOUT6 = o[6];
    assign CLKFBOUT  = CLKIN1;
    assign CLKFBOUTB = ~CLKIN1;
    assign LOCKED    = locked_r;
    assign DO        = 16'd0;
    assign DRDY      = drdy_r;
    assign PSDONE    = psdone_r;
    assign CLKFBSTOPPED = 1'b0;
    assign CLKINSTOPPED = 1'b0;
`endif
endmodule
/* verilator lint_on UNDRIVEN */
/* verilator lint_on UNUSED */
