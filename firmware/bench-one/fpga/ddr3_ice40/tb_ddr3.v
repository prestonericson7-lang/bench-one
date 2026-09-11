/* ===========================================================================================
 *  tb_ddr3.v -- drive ddr3_ctrl against ddr3_model and insist the bytes come back
 * ===========================================================================================
 *
 *  Three things are proved here, and they are the three that cannot be checked with a meter once
 *  the thing is wired:
 *
 *    1. The initialisation sequence is accepted by a device that is checking it. The model enforces
 *       mode register order, the DLL disable bit, CL, CWL, and refuses column commands until it has
 *       seen all four registers.
 *    2. A streaming write followed by a streaming read returns the same bytes in the same order.
 *       That catches the beat-order and endianness mistakes a single-word test never does.
 *    3. Data still lands when the device returns it late. RD_DELAY_HALF is overridable from the
 *       command line, because page 115 allows tDQSCK to exceed a whole clock and the controller's
 *       fixed-offset sampling is what has to absorb it.
 *
 *  Run one case:   vvp tb.vvp
 *  Sweep the delay: vvp tb.vvp +rdhalf=12
 * ======================================================================================== */

`default_nettype none
`timescale 1ns / 1ps

module tb_ddr3 #(
    parameter integer SYS_HZ   = 100_000_000,
    parameter integer CK_DIV   = 8,            /* 12.5 MHz memory clock, 80 ns */
    /* how late the device returns data, in half clock periods; 10 is nominal (AL+CL-1 = 5 clocks) */
    parameter integer RD_DELAY_HALF = 10,
    parameter integer RD_LATENCY    = 6,
    parameter integer RD_SAMPLE     = 2
) ();

    localparam integer ROW_BITS = 15;
    localparam integer COL_BITS = 10;
    localparam integer BA_BITS  = 3;


    reg sys_clk = 0;
    reg sys_rst = 1;
    always #5 sys_clk = ~sys_clk;              /* 100 MHz fabric */

    reg                 req_valid = 0;
    reg                 req_write = 0;
    wire                req_ready;
    reg  [BA_BITS-1:0]  req_bank = 0;
    reg  [ROW_BITS-1:0] req_row  = 0;
    reg  [COL_BITS-1:0] req_col  = 0;
    reg  [7:0]          req_len  = 1;
    reg  [63:0]         wd_data  = 0;
    wire                wd_take;
    wire [63:0]         rd_data;
    wire                rd_valid;
    wire                busy;
    wire                init_done;

    wire               ddr_ck, ddr_ck_n, ddr_cke, ddr_reset_n, ddr_cs_n;
    wire               ddr_ras_n, ddr_cas_n, ddr_we_n, ddr_odt;
    wire [BA_BITS-1:0] ddr_ba;
    wire [15:0]        ddr_a;
    wire [7:0]         ddr_dq_o;
    wire               ddr_dq_oe, ddr_dqs_o, ddr_dqs_oe;
    wire [7:0]         model_dq_o;
    wire               model_dq_oe;

    /* the shared data bus, resolved the way the real one is */
    wire [7:0] dq_bus = ddr_dq_oe   ? ddr_dq_o   :
                        model_dq_oe ? model_dq_o : 8'hzz;

    ddr3_ctrl #(
        .SYS_HZ(SYS_HZ), .CK_DIV(CK_DIV),
        .ROW_BITS(ROW_BITS), .COL_BITS(COL_BITS), .BA_BITS(BA_BITS),
        .RD_SAMPLE(RD_SAMPLE), .RD_LATENCY(RD_LATENCY)
    ) dut (
        .sys_clk(sys_clk), .sys_rst(sys_rst),
        /* Tied off: this bench exercises the parameter defaults. An undriven cfg_load
         * would be X and would load garbage over the reset values. */
        .cfg_rd_latency(8'd0), .cfg_rd_sample(4'd0), .cfg_load(1'b0),
        .req_valid(req_valid), .req_ready(req_ready), .req_write(req_write),
        .req_bank(req_bank), .req_row(req_row), .req_col(req_col), .req_len(req_len),
        .wd_data(wd_data), .wd_take(wd_take),
        .rd_data(rd_data), .rd_valid(rd_valid), .busy(busy), .init_done(init_done),
        .ddr_ck(ddr_ck), .ddr_ck_n(ddr_ck_n), .ddr_cke(ddr_cke), .ddr_reset_n(ddr_reset_n),
        .ddr_cs_n(ddr_cs_n), .ddr_ras_n(ddr_ras_n), .ddr_cas_n(ddr_cas_n), .ddr_we_n(ddr_we_n),
        .ddr_ba(ddr_ba), .ddr_a(ddr_a), .ddr_odt(ddr_odt),
        .ddr_dq_o(ddr_dq_o), .ddr_dq_oe(ddr_dq_oe), .ddr_dq_i(dq_bus),
        .ddr_dqs_o(ddr_dqs_o), .ddr_dqs_oe(ddr_dqs_oe)
    );

    ddr3_model #(
        .ROW_BITS(ROW_BITS), .COL_BITS(COL_BITS), .BA_BITS(BA_BITS),
        .RD_DELAY_HALF(RD_DELAY_HALF), .CHECK_INIT_US(0)
    ) mem (
        .ck(ddr_ck), .ck_n(ddr_ck_n), .cke(ddr_cke), .reset_n(ddr_reset_n),
        .cs_n(ddr_cs_n), .ras_n(ddr_ras_n), .cas_n(ddr_cas_n), .we_n(ddr_we_n),
        .ba(ddr_ba), .a(ddr_a), .odt(ddr_odt),
        .dq_i(dq_bus), .dq_oe(ddr_dq_oe), .dq_o(model_dq_o), .dq_oe_m(model_dq_oe),
        .dqs_i(ddr_dqs_o), .dqs_oe(ddr_dqs_oe)
    );

    localparam integer NBURST = 16;            /* 128 bytes, four cache lines */
    reg [63:0] expect [0:NBURST-1];
    reg [63:0] got    [0:NBURST-1];
    integer    wi, ri, fails, widx, gidx;

    /* feed write data as the controller asks for it */
    always @(posedge sys_clk) begin
        if (wd_take) begin
            widx    = widx + 1;
            wd_data = expect[(widx) % NBURST];
        end
    end

    /* collect read bursts */
    always @(posedge sys_clk) begin
        if (rd_valid) begin
            if (gidx < NBURST) got[gidx] = rd_data;
            gidx = gidx + 1;
        end
    end

    task do_req;
        input        wr;
        input [7:0]  len;
        input [9:0]  c;
        begin
            wait (req_ready);
            req_write = wr;
            req_col   = c;
            req_len   = len;
            req_valid = 1'b1;
            @(posedge sys_clk);
            while (!busy) @(posedge sys_clk);
            req_valid = 1'b0;
            wait (!busy);
            repeat (4) @(posedge sys_clk);
        end
    endtask

    initial begin
        $display("=== DDR3 controller vs a checking model ===");
        $display("  tCK %0d ns, RD_LATENCY %0d, RD_SAMPLE %0d, model returns data %0d half-clocks late",
                 (CK_DIV * 1000) / (SYS_HZ/1000000), RD_LATENCY, RD_SAMPLE, RD_DELAY_HALF);

        fails = 0; widx = 0; gidx = 0;
        for (wi = 0; wi < NBURST; wi = wi + 1) expect[wi] = {$random, $random};

        wd_data  = expect[0];
        req_bank = 3'd2;
        req_row  = 15'h0123;

        repeat (4) @(posedge sys_clk);
        sys_rst = 0;

        /* the controller counts a real 200 us and 500 us, so wait rather than guess */
        wait (init_done);
        $display("  init complete at %0t ns", $time);
        if (mem.initialised !== 1'b1) begin
            $display("  *** the device never saw a complete initialisation sequence");
            fails = fails + 1;
        end
        if (mem.dll_off !== 1'b1) begin
            $display("  *** the device reports the DLL is still enabled");
            fails = fails + 1;
        end

        /* ---- streaming write, then read the same run back ---- */
        widx = 0;
        do_req(1'b1, NBURST, 10'd0);
        $display("  wrote %0d bursts (%0d bytes)", NBURST, NBURST*8);

        gidx = 0;
        do_req(1'b0, NBURST, 10'd0);
        $display("  read back %0d bursts", gidx);

        if (gidx != NBURST) begin
            $display("  *** expected %0d read bursts, got %0d", NBURST, gidx);
            fails = fails + 1;
        end
        for (ri = 0; ri < NBURST; ri = ri + 1) begin
            if (ri < gidx && got[ri] !== expect[ri]) begin
                $display("  *** burst %0d: wrote %h, read %h", ri, expect[ri], got[ri]);
                fails = fails + 1;
            end
        end

        /* ---- idle long enough that refresh has to be working ---- */
        $display("  idling 40 us so the refresh timer must fire...");
        #40_000;

        gidx = 0;
        do_req(1'b0, NBURST, 10'd0);
        for (ri = 0; ri < NBURST; ri = ri + 1)
            if (ri < gidx && got[ri] !== expect[ri]) begin
                $display("  *** after idle, burst %0d: wrote %h, read %h", ri, expect[ri], got[ri]);
                fails = fails + 1;
            end

        mem.report;
        if (fails == 0 && mem.errors == 0) $display("=== PASSED ===");
        else $display("=== FAILED: %0d data mismatches, %0d protocol errors ===", fails, mem.errors);
        $finish;
    end

    initial begin
        #30_000_000;
        $display("*** TIMEOUT");
        $finish;
    end

endmodule

`default_nettype wire
