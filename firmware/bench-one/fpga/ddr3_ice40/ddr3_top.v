/* ===========================================================================================
 *  ddr3_top.v -- Alchitry Cu (iCE40-HX8K) top level: a Teensy on one side, a DIMM on the other
 * ===========================================================================================
 *
 *      Teensy 4.1 FlexSPI2                 iCE40-HX8K                      DDR3 DIMM
 *      pin 53 SCLK  ------------------>  sck                     CK, CK#  <---- 1.5 V
 *      pin 51 SS1   ------------------>  cs_n                    CKE, RESET#
 *      pin 52 IO0   <----------------->  io[0]                   CS#, RAS#, CAS#, WE#
 *      pin 49 IO1   <----------------->  io[1]                   BA0..2, A0..14
 *      pin 50 IO2   <----------------->  io[2]                   DQ0..7, DQS0 pair
 *      pin 54 IO3   <----------------->  io[3]
 *
 *  The Teensy's side is 3.3 V and so is the iCE40 bank, so those six lines connect directly. The
 *  DIMM's side is 1.5 V and every one of its 38 lines needs translation: plain resistor dividers
 *  are adequate for the output-only lines (clock, address, command, control), and the data and
 *  strobe lines are bidirectional and need real parts.
 *
 *  The PSRAM stays on chip select 0 and keeps working. This sits on chip select 1, which the Teensy
 *  core already configures as a second device and which is free on every Teensy 4.1 that has not had
 *  a second PSRAM soldered to it.
 * ======================================================================================== */

`default_nettype none
`timescale 1ns / 1ps

module ddr3_top #(
    /* ---- clocking ----------------------------------------------------------------------------
     * The board oscillator is 100 MHz and the fabric cannot run there. Measured on this design,
     * place-and-route closes between 81.5 and 97.4 MHz depending only on the placer seed: the best
     * seed very nearly makes 100 MHz, and a bitstream that works because of a lucky seed is not a
     * bitstream, so sys_clk comes from the PLL instead and the oscillator is only its reference.
     *
     * SYS_HZ is the POST-PLL frequency and everything downstream derives its timing from it.
     * USE_PLL 0 bypasses the primitive so the same source simulates, where clk is driven at SYS_HZ
     * directly.
     *
     * The two settings that are actually built, both from icepll with DIVR 0 so the phase detector
     * runs at the full 100 MHz:
     *
     *    62.5 MHz   DIVF  9, DIVQ 4, FILTER 5   CK_DIV 4 -> 15.625 MHz memory, 31.25 MB/s of DRAM
     *    50.0 MHz   DIVF  7, DIVQ 4, FILTER 5   CK_DIV 8 ->  6.25  MHz memory, 12.5  MB/s of DRAM
     * -------------------------------------------------------------------------------------- */
    parameter integer SYS_HZ     = 62_500_000,
    parameter integer USE_PLL    = 1,
    parameter integer PLL_DIVR   = 0,
    parameter integer PLL_DIVF   = 9,
    parameter integer PLL_DIVQ   = 4,
    parameter integer PLL_FILTER = 5,
    parameter integer CK_DIV     = 4,        /* 15.625 MHz memory at 62.5 MHz sys_clk */
    parameter integer RD_LATENCY = 6,
    parameter integer RD_SAMPLE  = 1,
    parameter integer LAT_CYCLES = 200,
    parameter integer BUF_BITS   = 10,
    /* Fabric clocks to hold reset after configuration, letting the DIMM's supplies settle before
     * the controller starts counting its 200 us. Shortened in simulation, where 655 us of settling
     * is 655 us of nothing happening. */
    parameter integer RST_HOLD   = 65535
) (
    input  wire        clk,          /* 100 MHz board oscillator (PLL reference) */
    input  wire        rst_n,        /* Alchitry reset button, active low */

    /* ---- Teensy FlexSPI2 side, 3.3 V, direct ---- */
    input  wire        qspi_sck,
    input  wire        qspi_cs_n,
    inout  wire [3:0]  qspi_io,

    /* ---- DDR3 side, all through level translation ---- */
    output wire        ddr_ck_p,
    output wire        ddr_ck_n,
    output wire        ddr_cke,
    output wire        ddr_reset_n,
    output wire        ddr_cs_n,
    output wire        ddr_ras_n,
    output wire        ddr_cas_n,
    output wire        ddr_we_n,
    output wire [2:0]  ddr_ba,
    output wire [14:0] ddr_a,
    output wire        ddr_odt,
    inout  wire [7:0]  ddr_dq,
    inout  wire        ddr_dqs,      /* DQS0,  contact 7 */
    inout  wire        ddr_dqs_n,    /* DQS0#, contact 6 -- see the note below */

    output wire        led_init,     /* DDR3 came up */
    output wire        led_act       /* a transfer is in flight */
);

    localparam integer ROW_BITS = 15;
    localparam integer COL_BITS = 10;
    localparam integer BA_BITS  = 3;

    /* ---- sys_clk ------------------------------------------------------------------------------
     * SB_PLL40_CORE rather than SB_PLL40_PAD: on the Cu the oscillator lands on P7, which is a global
     * buffer input and is already being used as an ordinary clock input by this design, so the
     * reference is taken from fabric. The extra jitter is irrelevant here -- the memory clock is
     * sys_clk divided by at least four, and the DIMM's own tCK window with the DLL disabled runs from
     * 8 ns all the way to 7.8 us. */
    wire sys_clk;
    wire pll_lock;

    generate
        if (USE_PLL != 0) begin : g_pll
            SB_PLL40_CORE #(
                .FEEDBACK_PATH("SIMPLE"),
                .PLLOUT_SELECT("GENCLK"),
                .DIVR(PLL_DIVR[3:0]),
                .DIVF(PLL_DIVF[6:0]),
                .DIVQ(PLL_DIVQ[2:0]),
                .FILTER_RANGE(PLL_FILTER[2:0])
            ) pll_i (
                .REFERENCECLK (clk),
                .PLLOUTCORE   (sys_clk),
                .LOCK         (pll_lock),
                .RESETB       (1'b1),
                .BYPASS       (1'b0)
            );
        end else begin : g_nopll
            /* Simulation and any board whose oscillator is already the wanted frequency. */
            assign sys_clk  = clk;
            assign pll_lock = 1'b1;
        end
    endgenerate

    /* Reset: hold for a while after configuration so the DIMM's supplies have settled before the
     * controller starts counting its 200 us. Also held until the PLL has locked, because until then
     * sys_clk is not at its final frequency and every timer derived from it would be wrong. */
    reg [15:0] rst_cnt = 16'd0;
    reg        sys_rst = 1'b1;
    always @(posedge sys_clk) begin
        if (!rst_n || !pll_lock) begin
            rst_cnt <= 16'd0;
            sys_rst <= 1'b1;
        end else if (rst_cnt != RST_HOLD[15:0]) begin
            rst_cnt <= rst_cnt + 1'b1;
        end else begin
            sys_rst <= 1'b0;
        end
    end

    /* ---- the buffers, two of them, and the reason there are two ------------------------------
     * An iCE40 block RAM is not a true dual-port memory. SB_RAM40_4K has ONE write port and ONE
     * read port, each with its own clock -- which is generous, and not the same thing. A single
     * buffer needing read and write access from both clock domains has no mapping onto this
     * hardware at all, and yosys says so rather than inventing one.
     *
     * Splitting by direction fits perfectly instead. Each buffer is written on one clock and read on
     * the other, which is exactly the primitive's shape:
     *
     *     rdbuf : DRAM fills it on clk,      the Teensy drains it on sck
     *     wrbuf : the Teensy fills it on sck, DRAM drains it on clk
     *
     * 1 KB each, so four of the HX8K's sixteen block RAMs. They also never contend, because a
     * transaction is either a read or a write, never both.
     * -------------------------------------------------------------------------------------- */
    reg [7:0] rdbuf [0:(1<<BUF_BITS)-1];
    reg [7:0] wrbuf [0:(1<<BUF_BITS)-1];

    wire [BUF_BITS-1:0] qa_addr;      /* sck side address, shared by both directions */
    wire [7:0]          qa_wdata;
    wire                qa_we;
    reg  [7:0]          qa_rdata;

    wire [BUF_BITS-1:0] qb_addr;      /* clk side address */
    wire [7:0]          qb_wdata;
    wire                qb_we;
    reg  [7:0]          qb_rdata;

    /* rdbuf: written by the bridge on clk, read by the QSPI slave on sck */
    always @(posedge sys_clk)  if (qb_we) rdbuf[qb_addr] <= qb_wdata;
    always @(posedge qspi_sck) qa_rdata <= rdbuf[qa_addr];

    /* wrbuf: written by the QSPI slave on sck, read by the bridge on clk */
    always @(posedge qspi_sck) if (qa_we) wrbuf[qa_addr] <= qa_wdata;
    always @(posedge sys_clk)  qb_rdata <= wrbuf[qb_addr];

    /* ---- QSPI slave, Teensy facing ---- */
    wire [3:0]  q_io_i;
    assign q_io_i = qspi_io;
    wire [3:0]  q_io_o;
    wire [3:0]  q_io_oe;
    wire [31:0] q_addr, q_win;
    wire        q_start, q_is_write, q_prefetch;
    wire [BUF_BITS-1:0] q_wrcount;
    wire [7:0]  q_lat;
    wire [3:0]  q_samp;
    wire        q_cfgstb;

    /* Bring the config toggle across from the sck domain: two flops to settle it, a third to see the
     * change against. This is the only place the two domains meet outside the buffers. */
    reg [2:0] cfg_sync = 3'b000;
    always @(posedge sys_clk) begin
        if (sys_rst) cfg_sync <= 3'b000;
        else         cfg_sync <= {cfg_sync[1:0], q_cfgstb};
    end
    wire q_cfgload = (cfg_sync[2] != cfg_sync[1]);

    /* The identity word carries the build's memory clock divider in its top byte.
     *
     * The Teensy has to know which CK_DIV the bitstream was built for: it decides the dummy-cycle
     * count and how long a transaction may be, and a mismatch produces fast, confident, wrong data
     * that looks exactly like a wiring fault. Rather than trust two numbers to be kept in step by
     * hand, the gateware reports its own. Bits 31 to 24 are unused by the Teensy core's probe, which
     * inspects only the low half and bits 23 to 21, and 0x00805D9D keeps those reading as a 16 MB
     * part whatever the top byte holds. */
    qspi_slave #(
        .LAT_CYCLES(LAT_CYCLES),
        .BUF_BITS(BUF_BITS),
        .CHIP_ID((CK_DIV << 24) | 32'h00805D9D)
    ) qs (
        .sck(qspi_sck), .rst(sys_rst), .cs_n(qspi_cs_n),
        .io_i(q_io_i), .io_o(q_io_o), .io_oe(q_io_oe),
        .buf_addr(qa_addr), .buf_rdata(qa_rdata),
        .buf_wdata(qa_wdata), .buf_we(qa_we),
        .req_addr(q_addr), .req_start(q_start), .req_is_write(q_is_write),
        .win_base(q_win), .prefetch_en(q_prefetch), .wr_count(q_wrcount),
        .cfg_rd_latency(q_lat), .cfg_rd_sample(q_samp), .cfg_stb(q_cfgstb)
    );

    /* Per-line tristate so a single-bit reply drives only IO1, exactly as a real part does.
     *
     * Written as plain tristate assignments rather than instantiated SB_IO primitives. yosys infers
     * the same SB_IO cells from this (checked: still 13 of them after synthesis), and it means the
     * whole design simulates with no vendor cell library, which is what makes the end-to-end
     * testbench possible at all. Instantiating the primitive directly bought nothing and cost the
     * ability to test. */
    assign qspi_io[0] = q_io_oe[0] ? q_io_o[0] : 1'bz;
    assign qspi_io[1] = q_io_oe[1] ? q_io_o[1] : 1'bz;
    assign qspi_io[2] = q_io_oe[2] ? q_io_o[2] : 1'bz;
    assign qspi_io[3] = q_io_oe[3] ? q_io_o[3] : 1'bz;

    /* ---- bridge ---- */
    wire                req_valid, req_ready, req_write;
    wire [BA_BITS-1:0]  req_bank;
    wire [ROW_BITS-1:0] req_row;
    wire [COL_BITS-1:0] req_col;
    wire [7:0]          req_len;
    wire [63:0]         wd_data, rd_data;
    wire                wd_take, rd_valid, init_done, br_idle, br_overrun, br_deferred, br_align;

    ddr3_bridge #(
        .ROW_BITS(ROW_BITS), .COL_BITS(COL_BITS), .BA_BITS(BA_BITS), .BUF_BITS(BUF_BITS),
        .SYS_HZ(SYS_HZ), .CK_DIV(CK_DIV)
    ) br (
        .sys_clk(sys_clk), .sys_rst(sys_rst),
        .q_start(q_start), .q_is_write(q_is_write), .q_addr(q_addr),
        .q_win_base(q_win), .q_cs_n(qspi_cs_n), .q_wr_count(q_wrcount),
        .buf_addr(qb_addr), .buf_wdata(qb_wdata), .buf_we(qb_we), .buf_rdata(qb_rdata),
        .req_valid(req_valid), .req_ready(req_ready), .req_write(req_write),
        .req_bank(req_bank), .req_row(req_row), .req_col(req_col), .req_len(req_len),
        .wd_data(wd_data), .wd_take(wd_take), .rd_data(rd_data), .rd_valid(rd_valid),
        .init_done(init_done), .idle(br_idle), .err_overrun(br_overrun),
        .err_deferred(br_deferred), .err_align(br_align)
    );

    /* ---- DDR3 controller ---- */
    wire [7:0] c_dq_o;
    wire [7:0] c_dq_i;
    wire       c_dq_oe, c_dqs_o, c_dqs_oe;
    wire       ck_int;
    /* The controller drives a full 16-bit address bus because mode register writes use the high
     * bits; a 15-row-bit device only wires A0..A14, so the top bit is simply not routed. An output
     * port cannot be tied to a concatenation containing a constant, hence the intermediate wire. */
    wire [15:0] a_int;

    ddr3_ctrl #(
        .SYS_HZ(SYS_HZ), .CK_DIV(CK_DIV),
        .ROW_BITS(ROW_BITS), .COL_BITS(COL_BITS), .BA_BITS(BA_BITS),
        .RD_SAMPLE(RD_SAMPLE), .RD_LATENCY(RD_LATENCY)
    ) ctl (
        .sys_clk(sys_clk), .sys_rst(sys_rst),
        /* The controller takes a clean single-cycle pulse in its own clock domain; the toggle from
         * the sck domain was turned into one by the synchroniser above. */
        .cfg_rd_latency(q_lat), .cfg_rd_sample(q_samp), .cfg_load(q_cfgload),
        .req_valid(req_valid), .req_ready(req_ready), .req_write(req_write),
        .req_bank(req_bank), .req_row(req_row), .req_col(req_col), .req_len(req_len),
        .wd_data(wd_data), .wd_take(wd_take),
        .rd_data(rd_data), .rd_valid(rd_valid), .busy(), .init_done(init_done),
        .ddr_ck(ck_int), .ddr_ck_n(), .ddr_cke(ddr_cke), .ddr_reset_n(ddr_reset_n),
        .ddr_cs_n(ddr_cs_n), .ddr_ras_n(ddr_ras_n), .ddr_cas_n(ddr_cas_n), .ddr_we_n(ddr_we_n),
        .ddr_ba(ddr_ba), .ddr_a(a_int), .ddr_odt(ddr_odt),
        .ddr_dq_o(c_dq_o), .ddr_dq_oe(c_dq_oe), .ddr_dq_i(c_dq_i),
        .ddr_dqs_o(c_dqs_o), .ddr_dqs_oe(c_dqs_oe)
    );

    assign ddr_a    = a_int[14:0];
    assign ddr_ck_p = ck_int;
    assign ddr_ck_n = ~ck_int;

    assign ddr_dq  = c_dq_oe  ? c_dq_o  : 8'hzz;
    assign c_dq_i  = ddr_dq;
    /* DQS is a DIFFERENTIAL pair on a DDR3 device and its write-data receiver is a differential
     * comparator. Driving only the true half and leaving the complement floating leaves that
     * comparator with no reference and write data that lands at random. Generating the complement
     * costs one translator channel and removes the guesswork. The alternative, biasing DQS# at VREF
     * to turn the receiver single-ended, also works but throws away half the noise margin on the one
     * signal that can least afford it. */
    assign ddr_dqs   = c_dqs_oe ?  c_dqs_o : 1'bz;
    assign ddr_dqs_n = c_dqs_oe ? ~c_dqs_o : 1'bz;

    assign led_init = init_done;
    /* Activity, or a latched overrun: if a burst ever arrived before the previous one was
     * stored, this stays lit. A silent dropped burst would look like data corruption. */
    assign led_act  = br_overrun | br_deferred | br_align | ~br_idle;

endmodule

`default_nettype wire
