// tb_pl_regs_clockstop.v -- what the FCLK0 experiment (linux/plx.py, board_experiment.py) assumes about
// pl_regs, checked on the RTL that is in the bitstream (pl_regs.v, CLK_HZ = 100 MHz as built):
//   1. the registers plx.py reads: ID 0x5A702001, CLK_HZ = 100000000, SCRATCH write/read-back, and
//      TIME_LO latching TIME_HI (a coherent 64-bit read);
//   2. the gate test: with the block's clock held low for G cycles, the 64-bit counter advances by exactly
//      (cycles elapsed - G): it stops while gated and resumes afterwards;
//   3. the stall test: an AXI read issued while the clock is held low gets NO response (no ARREADY, no
//      RVALID) for as long as it stays low, and completes, with correct data, once the clock runs again.
// The clock gate is modelled as FCLK0 held low (a glitch-free gate). What RTL cannot show: whether the
// PS's FPGA0_THR_CNT bit stops FCLK0 in silicon -- that is the board's gate test.
//   Vivado:  xvlog tb_pl_regs_clockstop.v pl_regs.v && xelab tb_pl_regs_clockstop -s cs && xsim cs -R
`timescale 1ns / 1ps
module tb_pl_regs_clockstop;
    reg clk = 0; always #5 clk = ~clk;                 // 100 MHz, as FCLK0
    reg run = 1;                                       // the gate: 0 = FCLK0 stopped (held low)
    reg gated_clk = 0;
    always @(clk) gated_clk = run ? clk : 1'b0;        // glitch-free while run changes on the low phase
    wire aclk = gated_clk;
    reg aresetn = 0;
    reg  [31:0] awaddr = 0; reg awvalid = 0; wire awready;
    reg  [31:0] wdata = 0;  reg [3:0] wstrb = 4'hF; reg wvalid = 0; wire wready;
    wire [1:0] bresp; wire bvalid; reg bready = 1;
    reg  [31:0] araddr = 0; reg arvalid = 0; wire arready;
    wire [31:0] rdata; wire [1:0] rresp; wire rvalid; reg rready = 1;
    wire led1, led2, fan_pwm;

    pl_regs #(.CLK_HZ(100_000_000)) dut (
        .aclk(aclk), .aresetn(aresetn),
        .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .led1(led1), .led2(led2), .key1_n(1'b1), .key2_n(1'b1), .fan_pwm(fan_pwm), .fan_tach(1'b0));

    integer fails = 0;
    task fail(input [8*80-1:0] m); begin $display("FAIL %0s", m); fails = fails + 1; end endtask
    // the master runs on the free clock (as the PS core does) and waits for the slave's handshakes
    task axi_read(input [31:0] a, output [31:0] d);
        begin
            @(posedge clk); araddr <= a; arvalid <= 1;
            @(posedge clk); while (!arready) @(posedge clk); arvalid <= 0;
            while (!rvalid) @(posedge clk); d = rdata;
            if (rresp !== 2'b00) fail("read response not OKAY");
            @(posedge clk);
        end
    endtask
    task axi_write(input [31:0] a, input [31:0] v);
        begin
            @(posedge clk); awaddr <= a; awvalid <= 1; wdata <= v; wvalid <= 1;
            fork
                begin @(posedge clk); while (!awready) @(posedge clk); awvalid <= 0; end
                begin @(posedge clk); while (!wready)  @(posedge clk); wvalid  <= 0; end
            join
            while (!bvalid) @(posedge clk);
            @(posedge clk);
        end
    endtask
    task ticks(output [63:0] t);
        reg [31:0] lo, hi;
        begin axi_read(32'h04, lo); axi_read(32'h08, hi); t = {hi, lo}; end
    endtask
    // hold the clock low between two free-clock low phases, for exactly g cycles
    task gate_for(input integer g);
        begin @(negedge clk); run = 0; repeat (g) @(posedge clk); @(negedge clk); run = 1; end
    endtask

    reg [31:0] v, lo1, hi1, hi2;
    reg [63:0] t0, t1, t2, t3;
    integer c0, c1, cyc, waited, g;
    integer cycles = 0; always @(posedge clk) cycles = cycles + 1;
    initial begin
        repeat (5) @(posedge clk); aresetn = 1; repeat (5) @(posedge clk);

        // 1. the registers plx.py reads
        axi_read(32'h00, v);  if (v !== 32'h5A70_2001) fail("ID");           else $display("  ok  ID = %h", v);
        axi_read(32'h20, v);  if (v !== 32'd100_000_000) fail("CLK_HZ");     else $display("  ok  CLK_HZ = %0d", v);
        axi_write(32'h1C, 32'h1234_ABCD); axi_read(32'h1C, v);
        if (v !== 32'h1234_ABCD) fail("SCRATCH"); else $display("  ok  SCRATCH write/read-back %h", v);
        // TIME_LO latches TIME_HI: HI read later must be the latched value, even if the counter moved on
        axi_read(32'h04, lo1); repeat (50) @(posedge clk); axi_read(32'h08, hi1);
        axi_read(32'h08, hi2);
        if (hi1 !== hi2) fail("TIME_HI not latched by the TIME_LO read"); else $display("  ok  TIME_HI held from the TIME_LO read");

        // 2. the gate test, as plx.py gatetest does it: counter, gate held, counter
        for (g = 200; g <= 5000; g = g * 5) begin
            ticks(t0); c0 = cycles;
            gate_for(g);
            ticks(t1); c1 = cycles;
            // the counter must fall short of the elapsed cycles by the gated cycles (+-4 for the reads' alignment)
            if (((c1 - c0) - (t1 - t0)) < g - 4 || ((c1 - c0) - (t1 - t0)) > g + 4)
                fail("counter did not stop for the gated cycles");
            else $display("  ok  gate %0d cycles: %0d cycles elapsed, counter advanced %0d (short by %0d)",
                          g, c1 - c0, t1 - t0, (c1 - c0) - (t1 - t0));
            ticks(t2); repeat (1000) @(posedge clk); ticks(t3);
            if ((t3 - t2) < 1000) fail("counter not running again after the gate");
        end

        // 3. the stall test: a read issued with the clock held low
        @(negedge clk); run = 0;
        @(posedge clk); araddr <= 32'h04; arvalid <= 1;
        waited = 0;
        repeat (3000) begin @(posedge clk); if (arready || rvalid) waited = -1; else if (waited >= 0) waited = waited + 1; end
        if (waited < 0) fail("the slave answered while its clock was stopped");
        else $display("  ok  read issued with the clock stopped: no ARREADY, no RVALID for %0d cycles", waited);
        @(negedge clk); run = 1; cyc = 0;
        while (!arready) begin @(posedge clk); cyc = cyc + 1; end
        arvalid <= 0;
        while (!rvalid) begin @(posedge clk); cyc = cyc + 1; end
        if (cyc > 10) fail("read did not complete promptly once the clock ran");
        else $display("  ok  clock running again: the read completed %0d cycles later, TIME_LO = %0d", cyc, rdata);
        @(posedge clk);

        if (fails == 0) $display("PASS  pl_regs clock-stop: registers, counter stops and resumes with its clock, reads stall and then complete");
        else $display("FAIL  pl_regs clock-stop: %0d failures", fails);
        $finish;
    end
endmodule
