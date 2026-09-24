// pl_regs.v -- the PL's register file on the Zynq PS's M_AXI_GP0, AXI4-Lite slave.
//
// This is the first thing Linux on the PS can touch in the fabric, and it is the time master
// the car system architecture asks for: a free-running 64-bit counter clocked by FCLK0 (100 MHz
// from the PS PLL) that every other PL block stamps its data with. Registers (byte offsets from
// the block base, 0x4000_0000 on M_AXI_GP0):
//
//   0x00  ID        RO  32'h5A70_2001  -- "ZA7020" v1; software checks this before trusting anything
//   0x04  TIME_LO   RO  counter[31:0]  } reading TIME_LO latches TIME_HI, so a 64-bit read is coherent
//   0x08  TIME_HI   RO  counter[63:32] } (read LO first, then HI)
//   0x0C  LED       RW  bit0 = LED2 (software), bit1 = 1 -> LED1 shows the heartbeat (default 1)
//   0x10  KEY       RO  bit0 = KEY1 pressed, bit1 = KEY2 pressed (active-high here; the pins are active-low)
//   0x14  FAN_DUTY  RW  0..100 percent, 25 kHz PWM on the fan pin (default 60)
//   0x18  FAN_RPM   RO  tach pulses in the last second x 30 (2 pulses/rev)
//   0x1C  SCRATCH   RW  anything -- a write/read-back test register
//   0x20  CLK_HZ    RO  the ACLK frequency this block was built for (parameter), so software
//                       converts TIME ticks to seconds without guessing
//
// AXI4-Lite only: one outstanding transaction, no bursts. The PS7's GP port is AXI3 and will issue
// single-beat transfers to a Lite slave; the top ties BID/RID to WID/ARID and asserts RLAST, the same
// way the openXC7 ps7 demo does.
`default_nettype none
`timescale 1ns / 1ps

module pl_regs #(
    parameter integer CLK_HZ = 100_000_000
)(
    input  wire        aclk,
    input  wire        aresetn,
    // AXI4-Lite
    input  wire [31:0] s_axi_awaddr,
    input  wire        s_axi_awvalid,
    output reg         s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output reg         s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [31:0] s_axi_araddr,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,
    // board
    output wire        led1,        // heartbeat (or off)
    output wire        led2,        // software bit
    input  wire        key1_n,
    input  wire        key2_n,
    output wire        fan_pwm,
    input  wire        fan_tach
);
    localparam [31:0] ID = 32'h5A70_2001;

    // ---------------- time master ----------------
    reg [63:0] tick = 64'd0;
    reg [31:0] time_hi_latch = 32'd0;
    always @(posedge aclk) tick <= tick + 64'd1;

    // ---------------- heartbeat ~1 Hz ----------------
    localparam integer HB_BIT = (CLK_HZ > 60_000_000) ? 26 : 25;   // 100 MHz / 2^27 = 0.75 Hz toggle
    reg [26:0] hb = 27'd0;
    always @(posedge aclk) hb <= hb + 27'd1;

    // ---------------- registers ----------------
    reg [1:0]  led_reg   = 2'b10;       // heartbeat on, LED2 off
    reg [6:0]  fan_duty  = 7'd60;
    reg [31:0] scratch   = 32'd0;

    // keys: two-flop sync, active-low pins -> active-high bits
    reg [1:0] key_s0 = 2'b11, key_s1 = 2'b11;
    always @(posedge aclk) begin key_s0 <= {key2_n, key1_n}; key_s1 <= key_s0; end
    wire [1:0] key_pressed = ~key_s1;

    // fan PWM 25 kHz, duty in percent
    // duty percent -> compare threshold as a fixed-point multiply (no divider, exact at 100 MHz:
    // 4000 ticks * 4096 / 100 = 163840, so duty * 163840 >> 12 == duty * 40; within 1 tick elsewhere)
    localparam integer PWM_PERIOD = CLK_HZ / 25_000;
    localparam integer PWM_SCALE  = (PWM_PERIOD * 4096 + 50) / 100;
    reg  [15:0] pwm_cnt = 16'd0;
    wire [31:0] pwm_thr = (fan_duty * PWM_SCALE) >> 12;
    always @(posedge aclk) pwm_cnt <= (pwm_cnt == PWM_PERIOD - 1) ? 16'd0 : pwm_cnt + 16'd1;
    assign fan_pwm = ({16'd0, pwm_cnt} < pwm_thr);

    // tach: rising edges per second x 30
    reg [1:0]  tach_s = 2'b00; reg tach_d = 1'b0;
    reg [15:0] pulses = 16'd0, rpm = 16'd0;
    reg [31:0] sec_cnt = 32'd0;
    always @(posedge aclk) begin
        tach_s <= {tach_s[0], fan_tach}; tach_d <= tach_s[1];
        if (sec_cnt == CLK_HZ - 1) begin
            sec_cnt <= 32'd0; rpm <= pulses * 16'd30; pulses <= 16'd0;
        end else begin
            sec_cnt <= sec_cnt + 32'd1;
            if (tach_s[1] & ~tach_d) pulses <= pulses + 16'd1;
        end
    end

    assign led1 = led_reg[1] ? hb[HB_BIT] : 1'b0;
    assign led2 = led_reg[0];

    // ---------------- AXI4-Lite write ----------------
    reg [31:0] awaddr_q = 32'd0;
    reg        aw_got = 1'b0, w_got = 1'b0;
    reg [31:0] wdata_q = 32'd0; reg [3:0] wstrb_q = 4'd0;
    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_awready <= 1'b0; s_axi_wready <= 1'b0; s_axi_bvalid <= 1'b0; s_axi_bresp <= 2'b00;
            aw_got <= 1'b0; w_got <= 1'b0;
            led_reg <= 2'b10; fan_duty <= 7'd60; scratch <= 32'd0;
        end else begin
            // accept address and data independently, act when both are in
            s_axi_awready <= !aw_got && s_axi_awvalid && !s_axi_bvalid;
            if (s_axi_awvalid && s_axi_awready) begin awaddr_q <= s_axi_awaddr; aw_got <= 1'b1; end
            s_axi_wready <= !w_got && s_axi_wvalid && !s_axi_bvalid;
            if (s_axi_wvalid && s_axi_wready) begin wdata_q <= s_axi_wdata; wstrb_q <= s_axi_wstrb; w_got <= 1'b1; end
            if (aw_got && w_got && !s_axi_bvalid) begin
                s_axi_bvalid <= 1'b1; s_axi_bresp <= 2'b00; aw_got <= 1'b0; w_got <= 1'b0;
                case (awaddr_q[7:2])
                    6'h03: if (wstrb_q[0]) led_reg  <= wdata_q[1:0];
                    6'h05: if (wstrb_q[0]) fan_duty <= (wdata_q[6:0] > 7'd100) ? 7'd100 : wdata_q[6:0];
                    6'h07: begin
                        if (wstrb_q[0]) scratch[7:0]   <= wdata_q[7:0];
                        if (wstrb_q[1]) scratch[15:8]  <= wdata_q[15:8];
                        if (wstrb_q[2]) scratch[23:16] <= wdata_q[23:16];
                        if (wstrb_q[3]) scratch[31:24] <= wdata_q[31:24];
                    end
                    default: ;                       // RO or unmapped: accepted, ignored (OKAY, like Xilinx GPIO)
                endcase
            end
            if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 1'b0;
        end
    end

    // ---------------- AXI4-Lite read ----------------
    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_arready <= 1'b0; s_axi_rvalid <= 1'b0; s_axi_rdata <= 32'd0; s_axi_rresp <= 2'b00;
        end else begin
            s_axi_arready <= s_axi_arvalid && !s_axi_rvalid && !s_axi_arready;
            if (s_axi_arvalid && s_axi_arready) begin
                s_axi_rvalid <= 1'b1; s_axi_rresp <= 2'b00;
                case (s_axi_araddr[7:2])
                    6'h00: s_axi_rdata <= ID;
                    6'h01: begin s_axi_rdata <= tick[31:0]; time_hi_latch <= tick[63:32]; end
                    6'h02: s_axi_rdata <= time_hi_latch;
                    6'h03: s_axi_rdata <= {30'd0, led_reg};
                    6'h04: s_axi_rdata <= {30'd0, key_pressed};
                    6'h05: s_axi_rdata <= {25'd0, fan_duty};
                    6'h06: s_axi_rdata <= {16'd0, rpm};
                    6'h07: s_axi_rdata <= scratch;
                    6'h08: s_axi_rdata <= CLK_HZ;
                    default: s_axi_rdata <= 32'hDEAD_0000 | {26'd0, s_axi_araddr[7:2]};
                endcase
            end
            if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
        end
    end
endmodule
`default_nettype wire
