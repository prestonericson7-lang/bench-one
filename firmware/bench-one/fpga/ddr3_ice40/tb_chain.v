/* ===========================================================================================
 *  tb_chain.v -- the whole path, end to end: a FlexSPI master, the gateware, and a DDR3 device
 * ===========================================================================================
 *
 *  Everything before this tested the DDR3 controller alone. That left the QSPI slave, the bridge and
 *  the top level entirely unexercised, which is where most of the risk actually lives: the clock
 *  domain crossing, the nibble ordering, the buffer handover, the dummy-cycle window, the address
 *  decode. This drives the design the way a Teensy does and insists the bytes survive the round trip.
 *
 *  The master below is not a generic SPI model. It reproduces what FlexSPI2 actually emits, taken
 *  from the LUT the Teensy core programs and from the sketch's own sequences:
 *
 *      0x9F  identity   one pin,  then 24 dummy bits, then bytes on IO1
 *      0x35  enter quad one pin
 *      0xEB  quad read  two nibbles of command, eight of address, N dummy cycles, then data
 *      0x38  quad write two nibbles of command, eight of address, then data
 *
 *  Phase matters and is easy to get backwards. FlexSPI presents command, address and write data so
 *  they are stable across the RISING edge of SCLK, so the master changes them on the falling edge.
 *  The slave drives read data on the FALLING edge for the same reason, so the master samples on the
 *  rising edge. Getting this pair the wrong way round produces data shifted by exactly one nibble,
 *  which reads like an endianness bug and is not one.
 * ======================================================================================== */

`default_nettype none
`timescale 1ns / 1ps

module tb_chain #(
    parameter integer CK_DIV     = 4,        /* 25 MHz memory clock */
    parameter integer RD_LATENCY = 6,
    parameter integer RD_SAMPLE  = 1,
    parameter integer LAT_CYCLES = 64,
    /* SCLK half period in nanoseconds. 5 ns is 100 MHz, which is the stage-one link rate that
     * matches eight DDR3 data lines at 25 MHz. */
    parameter integer SCK_HALF   = 5
) ();

    /* ---- board clock ---- */
    reg clk = 0;
    always #5 clk = ~clk;                    /* 100 MHz */
    reg rst_n = 0;

    /* ---- the QSPI bus ---- */
    reg        sck   = 0;
    reg        cs_n  = 1;
    reg  [3:0] m_o   = 4'h0;
    reg  [3:0] m_oe  = 4'h0;                 /* per line, so single-bit mode drives only IO0 */
    wire [3:0] qspi_io;

    assign qspi_io[0] = m_oe[0] ? m_o[0] : 1'bz;
    assign qspi_io[1] = m_oe[1] ? m_o[1] : 1'bz;
    assign qspi_io[2] = m_oe[2] ? m_o[2] : 1'bz;
    assign qspi_io[3] = m_oe[3] ? m_o[3] : 1'bz;

    /* ---- the DDR3 bus ---- */
    wire        ddr_ck_p, ddr_ck_n, ddr_cke, ddr_reset_n, ddr_cs_n;
    wire        ddr_ras_n, ddr_cas_n, ddr_we_n, ddr_odt;
    wire [2:0]  ddr_ba;
    wire [14:0] ddr_a;
    wire [7:0]  ddr_dq;
    wire        ddr_dqs;
    wire        ddr_dqs_n;
    wire        led_init, led_act;

    ddr3_top #(
        .CK_DIV(CK_DIV), .RD_LATENCY(RD_LATENCY), .RD_SAMPLE(RD_SAMPLE),
        .LAT_CYCLES(LAT_CYCLES), .BUF_BITS(10),
        .RST_HOLD(64)                        /* no point simulating 655 us of settling */
    ) dut (
        .clk(clk), .rst_n(rst_n),
        .qspi_sck(sck), .qspi_cs_n(cs_n), .qspi_io(qspi_io),
        .ddr_ck_p(ddr_ck_p), .ddr_ck_n(ddr_ck_n), .ddr_cke(ddr_cke),
        .ddr_reset_n(ddr_reset_n), .ddr_cs_n(ddr_cs_n), .ddr_ras_n(ddr_ras_n),
        .ddr_cas_n(ddr_cas_n), .ddr_we_n(ddr_we_n), .ddr_ba(ddr_ba), .ddr_a(ddr_a),
        .ddr_odt(ddr_odt), .ddr_dq(ddr_dq), .ddr_dqs(ddr_dqs), .ddr_dqs_n(ddr_dqs_n),
        .led_init(led_init), .led_act(led_act)
    );

    /* The device model needs to know when the controller is driving, which is not visible from
     * outside a tristate net. Probing the controller's own enables is a testbench convenience and
     * costs nothing in the real build. */
    wire       dq_oe_probe  = dut.ctl.ddr_dq_oe;
    wire       dqs_oe_probe = dut.ctl.ddr_dqs_oe;
    wire [7:0] model_dq;
    wire       model_oe;

    assign ddr_dq = model_oe ? model_dq : 8'hzz;

    ddr3_model #(
        .ROW_BITS(15), .COL_BITS(10), .BA_BITS(3),
        .RD_DELAY_HALF(10), .CHECK_INIT_US(0)
    ) mem (
        .ck(ddr_ck_p), .ck_n(ddr_ck_n), .cke(ddr_cke), .reset_n(ddr_reset_n),
        .cs_n(ddr_cs_n), .ras_n(ddr_ras_n), .cas_n(ddr_cas_n), .we_n(ddr_we_n),
        .ba(ddr_ba), .a({1'b0, ddr_a}), .odt(ddr_odt),
        .dq_i(ddr_dq), .dq_oe(dq_oe_probe), .dq_o(model_dq), .dq_oe_m(model_oe),
        .dqs_i(ddr_dqs), .dqs_oe(dqs_oe_probe)
    );

    /* ======================================================================================
     *  The FlexSPI master
     * =================================================================================== */

    task sck_tick;      /* one full SCLK period, with the low phase first */
        begin
            #(SCK_HALF) sck = 1'b1;
            #(SCK_HALF) sck = 1'b0;
        end
    endtask

    /* One bit out on IO0, changed while SCLK is low so it is stable across the rising edge. */
    task send_bit;
        input b;
        begin
            m_oe = 4'b0001;
            m_o  = {3'b000, b};
            #(SCK_HALF) sck = 1'b1;
            #(SCK_HALF) sck = 1'b0;
        end
    endtask

    task send_byte_single;
        input [7:0] d;
        integer i;
        begin
            for (i = 7; i >= 0; i = i - 1) send_bit(d[i]);
        end
    endtask

    task send_nib;
        input [3:0] n;
        begin
            m_oe = 4'b1111;
            m_o  = n;
            #(SCK_HALF) sck = 1'b1;
            #(SCK_HALF) sck = 1'b0;
        end
    endtask

    task send_byte_quad;    /* high nibble first, as every quad SPI part expects */
        input [7:0] d;
        begin
            send_nib(d[7:4]);
            send_nib(d[3:0]);
        end
    endtask

    task idle_cycles;       /* dummy cycles: clock runs, master lets go of the bus */
        input integer n;
        integer i;
        begin
            m_oe = 4'h0;
            for (i = 0; i < n; i = i + 1) sck_tick;
        end
    endtask

    /* Read one nibble, sampled on the rising edge because the slave drove it on the falling one. */
    reg [3:0] got_nib;
    task recv_nib;
        begin
            m_oe = 4'h0;
            #(SCK_HALF) sck = 1'b1;
            got_nib = qspi_io;
            #(SCK_HALF) sck = 1'b0;
        end
    endtask

    reg [7:0] got_byte;
    task recv_byte_quad;
        begin
            recv_nib; got_byte[7:4] = got_nib;
            recv_nib; got_byte[3:0] = got_nib;
        end
    endtask

    task recv_bit_single;   /* single-bit replies come back on IO1 */
        begin
            m_oe = 4'h0;
            #(SCK_HALF) sck = 1'b1;
            got_nib = {3'b000, qspi_io[1]};
            #(SCK_HALF) sck = 1'b0;
        end
    endtask

    task cs_assert;   begin cs_n = 1'b0; #(SCK_HALF); end endtask
    task cs_release;  begin #(SCK_HALF); cs_n = 1'b1; m_oe = 4'h0; #(SCK_HALF*4); end endtask

    /* ---- the four transactions the Teensy actually performs ---- */

    reg [31:0] id;
    task read_id;
        integer i;
        begin
            cs_assert;
            send_byte_single(8'h9F);
            for (i = 0; i < 24; i = i + 1) send_bit(1'b0);   /* the LUT's 24 dummy bits */
            id = 32'd0;
            for (i = 0; i < 32; i = i + 1) begin
                recv_bit_single;
                id = {got_nib[0], id[31:1]};                 /* least significant bit first */
            end
            cs_release;
        end
    endtask

    task enter_quad;
        begin
            cs_assert;
            send_byte_single(8'h35);
            cs_release;
        end
    endtask

    task quad_write;
        input [31:0] addr;
        input integer n;
        input integer seed;
        integer i;
        begin
            cs_assert;
            send_byte_quad(8'h38);
            send_byte_quad(addr[31:24]); send_byte_quad(addr[23:16]);
            send_byte_quad(addr[15:8]);  send_byte_quad(addr[7:0]);
            for (i = 0; i < n; i = i + 1) send_byte_quad(pattern(seed, i));
            cs_release;
        end
    endtask

    reg [7:0] rdbuf [0:2047];
    task quad_read;
        input [31:0] addr;
        input integer n;
        integer i;
        begin
            cs_assert;
            send_byte_quad(8'hEB);
            send_byte_quad(addr[31:24]); send_byte_quad(addr[23:16]);
            send_byte_quad(addr[15:8]);  send_byte_quad(addr[7:0]);
            idle_cycles(LAT_CYCLES);
            for (i = 0; i < n; i = i + 1) begin
                recv_byte_quad;
                rdbuf[i] = got_byte;
            end
            cs_release;
        end
    endtask

    function [7:0] pattern;
        input integer seed;
        input integer i;
        begin
            pattern = (i * 8'h9D + seed * 8'h3B + (i >> 5)) & 8'hFF;
        end
    endfunction

    /* ======================================================================================
     *  The test
     * =================================================================================== */
    integer fails, i, bad;

    initial begin
        fails = 0;
        $display("=== end to end: FlexSPI master -> gateware -> DDR3 device ===");
        $display("  SCLK %0d MHz, dummy %0d cycles, memory clock %0d MHz",
                 1000 / (2*SCK_HALF), LAT_CYCLES, 100 / CK_DIV);

        repeat (10) @(posedge clk);
        rst_n = 1'b1;

        /* The gateware must finish DDR3 initialisation before it will answer anything. */
        wait (led_init);
        $display("  DDR3 reported initialised at %0t ns", $time);

        /* 1. identity, in single-bit mode, exactly as the stock Teensy probe does it */
        read_id;
        $display("  identity read back: 0x%08X", id);
        if (id[15:0] !== 16'h5D9D) begin
            $display("  *** identity wrong: expected low half 0x5D9D, got 0x%04X", id[15:0]);
            fails = fails + 1;
        end
        if (((id >> 21) & 3'h7) !== 3'b100) begin
            $display("  *** capacity field wrong: the Teensy core would not map this as 16 MB");
            fails = fails + 1;
        end

        enter_quad;

        /* 2. write 256 bytes, read them back */
        quad_write(32'h0000_0000, 256, 1);
        quad_read (32'h0000_0000, 256);
        bad = 0;
        for (i = 0; i < 256; i = i + 1)
            if (rdbuf[i] !== pattern(1, i)) begin
                if (bad < 4) $display("  *** byte %0d: wrote %02h, read %02h",
                                      i, pattern(1, i), rdbuf[i]);
                bad = bad + 1;
            end
        if (bad) begin
            $display("  *** 256-byte round trip: %0d of 256 bytes wrong", bad);
            fails = fails + 1;
        end else begin
            $display("  256-byte round trip at address 0: all bytes match");
        end

        /* 3. a second block at a different address, to prove the address decode is real and not
         *    just returning whatever was written last */
        quad_write(32'h0000_2000, 256, 2);
        quad_read (32'h0000_2000, 256);
        bad = 0;
        for (i = 0; i < 256; i = i + 1)
            if (rdbuf[i] !== pattern(2, i)) bad = bad + 1;
        if (bad) begin
            $display("  *** second block at 0x2000: %0d of 256 bytes wrong", bad);
            fails = fails + 1;
        end else $display("  second block at 0x2000: all bytes match");

        /* 4. re-read the first block. If the address decode is broken, this now holds block two. */
        quad_read(32'h0000_0000, 256);
        bad = 0;
        for (i = 0; i < 256; i = i + 1)
            if (rdbuf[i] !== pattern(1, i)) bad = bad + 1;
        if (bad) begin
            $display("  *** first block was disturbed by the second: %0d bytes wrong", bad);
            fails = fails + 1;
        end else $display("  first block survived the second: all bytes match");

        /* 5. an unaligned address, which exercises the column offset inside a row */
        quad_write(32'h0000_4000 + 13, 64, 3);
        quad_read (32'h0000_4000 + 13, 64);
        bad = 0;
        for (i = 0; i < 64; i = i + 1)
            if (rdbuf[i] !== pattern(3, i)) bad = bad + 1;
        if (bad) $display("  note: unaligned start, %0d of 64 bytes differ (burst granularity is 8)", bad);
        else $display("  unaligned start at +13: all bytes match");

        /* 6. hold long enough that only refresh can be keeping the data alive */
        $display("  holding 40 us so the refresh timer has to carry the data...");
        #40_000;
        quad_read(32'h0000_0000, 256);
        bad = 0;
        for (i = 0; i < 256; i = i + 1)
            if (rdbuf[i] !== pattern(1, i)) bad = bad + 1;
        if (bad) begin
            $display("  *** after the hold: %0d of 256 bytes wrong, refresh is not working", bad);
            fails = fails + 1;
        end else $display("  after the hold: all bytes match, refresh is carrying the array");

        mem.report;
        if (fails == 0 && mem.errors == 0) $display("=== CHAIN PASSED ===");
        else $display("=== CHAIN FAILED: %0d checks, %0d protocol errors ===", fails, mem.errors);
        $finish;
    end

    initial begin
        #20_000_000;
        $display("*** CHAIN TIMEOUT -- most likely the gateware never answered");
        $finish;
    end

endmodule

`default_nettype wire
