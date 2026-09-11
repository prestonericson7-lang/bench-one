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
    /* ---- fabric clock -------------------------------------------------------------------------
     * sys_clk is NOT the board oscillator. The Cu's oscillator is 100 MHz and this design does not
     * close there -- measured, across eight placer seeds, it lands between 81.5 and 97.4 MHz -- so the
     * real build takes sys_clk from the PLL and the testbench drives that frequency directly with
     * USE_PLL 0. SYS_HALF is half its period in nanoseconds.
     *
     *    62.5 MHz (SYS_HALF 8)  with CK_DIV 4 -> 15.625 MHz memory, 31.25 MB/s of DRAM
     *    50.0 MHz (SYS_HALF 10) with CK_DIV 8 ->  6.25  MHz memory, 12.5  MB/s of DRAM
     * -------------------------------------------------------------------------------------- */
    parameter integer SYS_HZ     = 62_500_000,
    parameter integer SYS_HALF   = 8,
    parameter integer CK_DIV     = 4,        /* 15.625 MHz memory clock */
    parameter integer RD_LATENCY = 6,
    parameter integer RD_SAMPLE  = 1,
    /* Dummy cycles. Must cover the first chunk reaching the buffer: ACTIVATE, tRCD, CAS latency and
     * sixteen bursts. 64 leaves the reader starting before the first chunk has landed and it never
     * recovers, showing up as a handful of wrong bytes at the tail. 120 clears it with margin. */
    parameter integer LAT_CYCLES = 200,
    /* SCLK half period in nanoseconds, and the single most important number in this file.
     *
     * THE RATE RULE. FlexSPI cannot be stalled once a read has started, so the DRAM must supply the
     * buffer faster than the link drains it, with real margin for the per-request overhead -- take
     * 15%. DRAM delivers CK_MHz x 2 MB/s on eight lines. The link delivers SCK_MHz / 2 MB/s on four
     * lines and SCK_MHz / 8 on one.
     *
     * This testbench once used 5 ns: a 100 MHz link worth 50 MB/s against a 25 MHz memory also worth
     * 50 MB/s. Zero margin, so the reader ran ahead of the data, and the failure looked exactly like a
     * buffer bug.
     *
     * 10 ns is 50 MHz, which happens to suit BOTH shipped configurations:
     *    four lines, 25 MB/s   against 31.25 MB/s of DRAM at 15.625 MHz   ->  1.25x
     *    one line,  6.25 MB/s  against 12.5  MB/s of DRAM at  6.25  MHz   ->  2.00x
     * It is also FlexSPI2's slowest setting, 396/8, so the Teensy needs no unusual divider. */
    parameter integer SCK_HALF   = 10,

    /* The quad cases need a memory clock of at least 14.4 MHz: four lines at FlexSPI2's slowest
     * setting consume 25 MB/s and the rate rule then wants 28.75 MB/s of DRAM behind them. Below that
     * only the single-bit path is valid, so set this to 0 and the quad cases are skipped rather than
     * failing for a reason that is a property of the configuration and not a bug. */
    parameter integer QUAD_OK    = 1
) ();

    /* ---- board clock ---- */
    reg clk = 0;
    always #SYS_HALF clk = ~clk;             /* this is sys_clk: the DUT is built with USE_PLL 0 */
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
        .SYS_HZ(SYS_HZ), .USE_PLL(0),
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

    /* Leaving quad mode is sent AS two nibbles, because the device is still in quad mode when it
     * arrives. A single-bit command cannot be decoded while quad mode is active, which is correct: a
     * driver picks one width and stays there. */
    task exit_quad;
        begin
            cs_assert;
            send_byte_quad(8'hF5);
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

    /* Single-bit read, command 0x03: one bit per clock out on IO0, one bit per clock back on IO1.
     * This is the path that makes a slow memory clock viable, because FlexSPI2 cannot be clocked
     * below 49.5 MHz and four lines at that speed outrun any memory slower than 14.4 MHz. */
    task single_read;
        input [31:0] addr;
        input integer n;
        integer i, b;
        begin
            cs_assert;
            send_byte_single(8'h03);
            for (i = 31; i >= 0; i = i - 1) send_bit(addr[i]);
            idle_cycles(LAT_CYCLES);
            for (i = 0; i < n; i = i + 1) begin
                got_byte = 8'h00;
                for (b = 0; b < 8; b = b + 1) begin
                    recv_bit_single;
                    got_byte = {got_byte[6:0], got_nib[0]};   /* most significant bit first */
                end
                rdbuf[i] = got_byte;
            end
            cs_release;
        end
    endtask
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

    task show;                      /* first eight expected against first eight returned */
        input integer seed;
        input integer off;
        integer k;
        begin
            $write("      expected ");
            for (k = 0; k < 8; k = k + 1) $write("%02h ", pattern(seed, k + off));
            $display("");
            $write("      got      ");
            for (k = 0; k < 8; k = k + 1) $write("%02h ", rdbuf[k]);
            $display("");
        end
    endtask

    initial begin
        fails = 0;
        $display("=== end to end: FlexSPI master -> gateware -> DDR3 device ===");
        /* Printed in kHz because the interesting configurations land on fractions of a megahertz:
         * 15625 kHz and 6250 kHz, not 25 and 6. Truncating those to whole MHz is how the banner came
         * to claim a 25 MHz memory clock on a build that was running at 15.625. */
        $display("  fabric %0d kHz, CK_DIV %0d -> memory %0d kHz, DRAM %0d kB/s",
                 SYS_HZ/1000, CK_DIV, (SYS_HZ/1000)/CK_DIV, 2*((SYS_HZ/1000)/CK_DIV));
        $display("  SCLK %0d MHz, %0d data line%s -> link %0d kB/s, dummy %0d cycles",
                 1000 / (2*SCK_HALF), QUAD_OK ? 4 : 1, QUAD_OK ? "s" : "",
                 QUAD_OK ? (1000/(2*SCK_HALF))*1000/2 : (1000/(2*SCK_HALF))*1000/8, LAT_CYCLES);

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

      /* The quad cases below need a memory clock of 14.4 MHz or better. Four lines at the slowest
       * consume 24.75 MB/s and the rate rule then wants 28.75 MB/s of DRAM behind them, so below that
       * they are skipped rather than failing for a reason that is a property of the configuration. */
      if (QUAD_OK) begin
        /* 2. write 256 bytes, read them back */
        quad_write(32'h0000_0000, 256, 1);
        #30_000;   /* let the write finish draining to DRAM before asking anything else */
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
         *    just returning whatever was written last.
         *
         * The gap matters and is not a fudge. A 256-byte write is 32 bursts, 5.1 us of DRAM time
         * after chip select rises, and no dummy-cycle count can cover that -- the field caps at 255
         * cycles. A driver issuing a large write followed immediately by a read has to leave the gap.
         * A Teensy does this naturally, because writes reach the bridge as cache-line writebacks of
         * 32 bytes, which drain in 640 ns against a 2.4 us window. Case 7 below checks that. */
        quad_write(32'h0000_2000, 256, 2);
        #30_000;
        quad_read (32'h0000_2000, 256);
        bad = 0;
        for (i = 0; i < 256; i = i + 1)
            if (rdbuf[i] !== pattern(2, i)) bad = bad + 1;
        if (bad) begin
            $display("  *** second block at 0x2000: %0d of 256 bytes wrong", bad);
            show(2, 0);
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

        /* 5. an unaligned READ. The bridge discards the leading bytes of the first burst so that
         *    buffer[0] is the byte actually asked for. Write aligned, read from the middle. */
        quad_write(32'h0000_4000, 64, 3);
        #30_000;
        quad_read (32'h0000_4000 + 13, 32);
        bad = 0;
        for (i = 0; i < 32; i = i + 1)
            if (rdbuf[i] !== pattern(3, i + 13)) bad = bad + 1;
        if (bad) begin
            $display("  *** unaligned read at +13: %0d of 32 bytes wrong", bad);
            show(3, 13);
            fails = fails + 1;
        end else $display("  unaligned read at +13: all bytes match");

        /* 6. an unaligned WRITE must be REFUSED, not silently misplaced. Writing a partial burst
         *    needs the data mask and DM is tied low here, so every byte of a burst is committed.
         *    The bridge latches err_align and aligns down, which is visible rather than silent. */
        quad_write(32'h0000_6000 + 3, 32, 4);
        #30_000;
        if (dut.br_align !== 1'b1) begin
            $display("  *** an unaligned write was accepted without raising err_align");
            fails = fails + 1;
        end else $display("  unaligned write correctly flagged by err_align");

        /* 7. back to back with NO gap, at the size a Teensy actually emits: a 32-byte cache line.
         *    This is the case that has to work unaided, and the one the driver relies on. */
        quad_write(32'h0000_8000, 32, 5);
        quad_read (32'h0000_8000, 32);
        bad = 0;
        for (i = 0; i < 32; i = i + 1)
            if (rdbuf[i] !== pattern(5, i)) bad = bad + 1;
        if (bad) begin
            $display("  *** 32-byte write then immediate read: %0d of 32 bytes wrong", bad);
            show(5, 0);
            fails = fails + 1;
        end else $display("  32-byte write then immediate read, no gap: all bytes match");

      end
        /* 5c. the single-bit read path. At a slow memory clock this is the ONLY valid path, and the
         *     quad cases above are skipped; at 25 MHz both work and must agree byte for byte. The two must
         *     return identical bytes: the shifting differs, nothing else does. */
        quad_write(32'h000A_0000, 64, 9);
        #30_000;
        exit_quad;
        single_read(32'h000A_0000, 32);
        bad = 0;
        for (i = 0; i < 32; i = i + 1)
            if (rdbuf[i] !== pattern(9, i)) bad = bad + 1;
        if (bad) begin
            $display("  *** single-bit read: %0d of 32 bytes wrong", bad);
            show(9, 0);
            fails = fails + 1;
        end else $display("  single-bit read at 0x0A0000: all bytes match");
        $write("      model row 80 col 0..7: ");
        for (i = 0; i < 8; i = i + 1) $write("%02h ", mem.mem[mem.cidx(3'd0, 15'd80, i[9:0])]);
        $display("");
        enter_quad;

      /* The checks below re-read the block the quad cases wrote, so they only mean anything when
       * those ran. With a slow memory clock the single-bit case above is the whole test. */
      if (QUAD_OK) begin
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

        /* Read the device model's array directly, bypassing the whole read path, so a mismatch
         * can be attributed to the write path or the read path rather than to "somewhere". */
        /* QSPI address 0 decodes to bank 0, row 0, column 0: addr[9:3] is the burst within the row,
         * addr[12:10] the bank, addr[27:13] the row. */
        $write("  model array at bank 0 row 0 col 0..15:      ");
        for (i = 0; i < 16; i = i + 1) $write("%02h ", mem.mem[mem.cidx(3'd0, 15'h0000, i[9:0])]);
        $display("");
        $write("  the pattern that was written:              ");
        for (i = 0; i < 16; i = i + 1) $write("%02h ", pattern(1, i));
        $display("");
        /* Does the whole written block match, not just the first sixteen bytes? */
        bad = 0;
        for (i = 0; i < 256; i = i + 1)
            if (mem.mem[mem.cidx(3'd0, 15'h0000, i[9:0])] !== pattern(1, i)) bad = bad + 1;
        $display("  all 256 stored bytes: %0d wrong", bad);
        for (i = 0; i < 256; i = i + 1)
            if (mem.mem[mem.cidx(3'd0, 15'h0000, i[9:0])] !== pattern(1, i))
                $display("    stored byte %0d: wrote %02h, array holds %02h", i,
                         pattern(1, i), mem.mem[mem.cidx(3'd0, 15'h0000, i[9:0])]);
        $write("  what the read path returned, bytes 0..15:   ");
        for (i = 0; i < 16; i = i + 1) $write("%02h ", rdbuf[i]);
        $display("");
      end
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
