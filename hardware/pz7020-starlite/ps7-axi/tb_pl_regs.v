// tb_pl_regs.v -- AXI4-Lite master BFM exercising every register of pl_regs.v.
//   iverilog -g2012 -o /tmp/plr.vvp tb_pl_regs.v pl_regs.v && vvp /tmp/plr.vvp
`timescale 1ns / 1ps
module tb_pl_regs;
    localparam integer CLK_HZ = 1_000_000;     // small so the 1-second tach window is simulable
    reg aclk = 0; always #5 aclk = ~aclk;      // "1 MHz" in the block's eyes, 100 MHz wall clock
    reg aresetn = 0;
    reg  [31:0] awaddr = 0; reg awvalid = 0; wire awready;
    reg  [31:0] wdata = 0;  reg [3:0] wstrb = 4'hF; reg wvalid = 0; wire wready;
    wire [1:0] bresp; wire bvalid; reg bready = 1;
    reg  [31:0] araddr = 0; reg arvalid = 0; wire arready;
    wire [31:0] rdata; wire [1:0] rresp; wire rvalid; reg rready = 1;
    wire led1, led2, fan_pwm; reg key1_n = 1, key2_n = 1, fan_tach = 0;

    pl_regs #(.CLK_HZ(CLK_HZ)) dut (
        .aclk(aclk), .aresetn(aresetn),
        .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .led1(led1), .led2(led2), .key1_n(key1_n), .key2_n(key2_n), .fan_pwm(fan_pwm), .fan_tach(fan_tach));

    integer fails = 0;
    task axi_write(input [31:0] a, input [31:0] d, input [3:0] s);
        begin
            @(posedge aclk); awaddr <= a; awvalid <= 1; wdata <= d; wstrb <= s; wvalid <= 1;
            fork
                begin @(posedge aclk); while (!awready) @(posedge aclk); awvalid <= 0; end
                begin @(posedge aclk); while (!wready)  @(posedge aclk); wvalid  <= 0; end
            join
            while (!bvalid) @(posedge aclk);
            if (bresp !== 2'b00) begin $display("FAIL write resp %b at %h", bresp, a); fails = fails + 1; end
            @(posedge aclk);
        end
    endtask
    task axi_read(input [31:0] a, output [31:0] d);
        begin
            @(posedge aclk); araddr <= a; arvalid <= 1;
            @(posedge aclk); while (!arready) @(posedge aclk); arvalid <= 0;
            while (!rvalid) @(posedge aclk); d = rdata;
            if (rresp !== 2'b00) begin $display("FAIL read resp %b at %h", rresp, a); fails = fails + 1; end
            @(posedge aclk);
        end
    endtask
    task check(input [31:0] a, input [31:0] want, input [255:0] what);
        reg [31:0] got;
        begin
            axi_read(a, got);
            if (got !== want) begin $display("FAIL %0s: read %h want %h", what, got, want); fails = fails + 1; end
            else $display("  ok  %0s = %h", what, got);
        end
    endtask

    reg [31:0] lo1, hi1, lo2, hi2, v;
    integer i, hi_pulses, lo_pulses;
    initial begin
        repeat (5) @(posedge aclk); aresetn = 1; repeat (2) @(posedge aclk);

        $display("1) identity and defaults");
        check(32'h00, 32'h5A70_2001, "ID");
        check(32'h20, CLK_HZ,        "CLK_HZ");
        check(32'h0C, 32'h2,         "LED default (heartbeat on, LED2 off)");
        check(32'h14, 32'd60,        "FAN_DUTY default");
        check(32'h10, 32'h0,         "KEY none pressed");

        $display("2) time master: 64-bit read is coherent and monotonic");
        axi_read(32'h04, lo1); axi_read(32'h08, hi1);
        repeat (50) @(posedge aclk);
        axi_read(32'h04, lo2); axi_read(32'h08, hi2);
        if ({hi2, lo2} <= {hi1, lo1}) begin $display("FAIL time not monotonic"); fails = fails + 1; end
        else $display("  ok  time %0d -> %0d ticks", {hi1, lo1}, {hi2, lo2});
        // force a carry across the 32-bit boundary and check HI latches with LO
        force dut.tick = 64'h0000_0000_FFFF_FFF0; @(posedge aclk); release dut.tick;
        repeat (20) @(posedge aclk);
        axi_read(32'h04, lo1); axi_read(32'h08, hi1);
        if (hi1 !== 32'h1 || lo1 > 32'h100) begin $display("FAIL carry: hi=%h lo=%h", hi1, lo1); fails = fails + 1; end
        else $display("  ok  carry crossed: hi=%h lo=%h (HI latched at the LO read)", hi1, lo1);

        $display("3) writable registers, byte strobes, clamp");
        axi_write(32'h1C, 32'hA5A5_5A5A, 4'hF); check(32'h1C, 32'hA5A5_5A5A, "SCRATCH full write");
        axi_write(32'h1C, 32'h0000_00FF, 4'h1); check(32'h1C, 32'hA5A5_5AFF, "SCRATCH byte-0 strobe only");
        axi_write(32'h0C, 32'h1, 4'hF);          check(32'h0C, 32'h1, "LED: LED2 on, heartbeat off");
        if (led2 !== 1'b1 || led1 !== 1'b0) begin $display("FAIL LED pins: led1=%b led2=%b", led1, led2); fails = fails + 1; end
        else $display("  ok  LED pins follow the register");
        axi_write(32'h14, 32'd250, 4'hF);        check(32'h14, 32'd100, "FAN_DUTY clamped to 100");
        axi_write(32'h14, 32'd25, 4'hF);         check(32'h14, 32'd25,  "FAN_DUTY 25");
        axi_write(32'h00, 32'hFFFF_FFFF, 4'hF);  check(32'h00, 32'h5A70_2001, "ID unchanged by a write (RO)");

        $display("4) PWM duty measured on the pin: 25 percent of a 40-tick period");
        hi_pulses = 0; lo_pulses = 0;
        repeat (400) begin @(posedge aclk); if (fan_pwm) hi_pulses = hi_pulses + 1; else lo_pulses = lo_pulses + 1; end
        if (hi_pulses < 80 || hi_pulses > 120) begin $display("FAIL PWM high %0d/400", hi_pulses); fails = fails + 1; end
        else $display("  ok  PWM high %0d of 400 ticks (~25 percent)", hi_pulses);

        $display("5) keys are active-low pins, active-high bits");
        key1_n = 0; repeat (4) @(posedge aclk); check(32'h10, 32'h1, "KEY1 pressed");
        key1_n = 1; key2_n = 0; repeat (4) @(posedge aclk); check(32'h10, 32'h2, "KEY2 pressed");
        key2_n = 1;

        $display("6) tach: 40 pulses in one window -> 1200 rpm");
        for (i = 0; i < 40; i = i + 1) begin fan_tach = 1; repeat (3) @(posedge aclk); fan_tach = 0; repeat (3) @(posedge aclk); end
        repeat (CLK_HZ + 10) @(posedge aclk);
        axi_read(32'h18, v);
        if (v !== 32'd1200) begin $display("FAIL rpm %0d", v); fails = fails + 1; end else $display("  ok  FAN_RPM = %0d", v);

        $display("7) unmapped address reads DEAD_xx, never hangs");
        check(32'h40, 32'hDEAD_0010, "unmapped 0x40");

        if (fails == 0) $display("PASS  pl_regs: all registers, time master, PWM, tach, keys verified");
        else $display("FAIL  pl_regs: %0d failures", fails);
        $finish;
    end
endmodule
