/* ===========================================================================================
 *  ddr3_ctrl.v -- DDR3 controller for iCE40, running a commodity DIMM far below its rated clock
 * ===========================================================================================
 *
 *  ROLE: a wire, not a worker. The Teensy does every arithmetic operation in BENCH ONE. This
 *  exists only so the Teensy can reach 4 GB of DDR3 instead of 16 MB of PSRAM. Nothing here
 *  unpacks, accumulates or reorders anything, so throughput measured through it is the Teensy's
 *  own number.
 *
 *  THE RATE IT MUST BEAT: FlexSPI2 on four data lines at 133 MHz is 66.5 MB/s. That is why reads
 *  stream -- one ACTIVATE then many READs down an open row, not ACT/READ/PRE per burst.
 *
 *      8 bytes per BL8 burst, 4 memory clocks per burst back to back:
 *      CK_DIV 8 -> 12.5 MHz ->  25 MB/s   bring-up only, would starve the link
 *      CK_DIV 4 -> 25.0 MHz ->  50 MB/s   still short
 *      CK_DIV 2 -> 50.0 MHz -> 100 MB/s   the target
 *
 *  WHY A SLOW CLOCK IS LEGAL: DDR3 specifies a DLL disable mode. Micron's 2Gb datasheet gives
 *  tCK(DLL_DIS) as 8 ns min and 7800 ns max at TC <= 85 C, so 20 ns is mid-window. Every timing
 *  quoted in nanoseconds (tRCD 13.75, tRP 13.75, tRAS 35, tRFC 160) is a MINIMUM and a slow clock
 *  satisfies them in one or two cycles. Refresh does not relax: 8192 per 64 ms, one per 7.8 us.
 *
 *  THE SURPRISE, DATASHEET PAGE 115: only CL=6 and CWL=6 exist in this mode; tDQSCK begins
 *  AL+CL-1 after READ; "with the DLL disabled, the value of tDQSCK could be larger than tCK";
 *  ODT unsupported so Rtt_Nom and Rtt_WR must be zero; Micron does not warrant the mode.
 *
 *  That third clause decides the read path: the returned strobe can arrive more than a whole clock
 *  late, so READ DATA IS NOT LATCHED ON DQS. It is sampled at a fixed offset (RD_SAMPLE), tuned on
 *  hardware. Writes are the opposite -- the DRAM must capture on the strobe WE generate, so DQS is
 *  driven out with a preamble and data is centred on its edges.
 *
 *  NOT DONE: no ZQ calibration (nothing is terminated at this speed), no bank interleaving, no
 *  write levelling (unused with the DLL off), one rank.
 * ======================================================================================== */

`default_nettype none
`timescale 1ns / 1ps

module ddr3_ctrl #(
    parameter integer SYS_HZ     = 100_000_000,
    parameter integer CK_DIV     = 8,
    parameter integer ROW_BITS   = 15,
    parameter integer COL_BITS   = 10,
    parameter integer BA_BITS    = 3,
    parameter integer RD_SAMPLE  = 2,

    /* Memory clocks from the controller ISSUING a READ to its first data beat.
     *
     * The datasheet's figure is AL + CL - 1 = 5 with AL 0 and CL 6, but that is measured from the
     * edge on which the DEVICE latches the command. Commands here are registered onto the pins on
     * the falling edge, deliberately, to buy half a memory period of setup -- so the device sees a
     * command one full clock after this controller issues it internally. The internal figure is
     * therefore 5 + 1 = 6. Setting it to the datasheet's 5 samples one clock early and returns the
     * previous burst's last two bytes ahead of every word, which is a very convincing wrong answer.
     * Raise it further only if the real part's tDQSCK turns out to exceed a whole clock, which page
     * 115 permits; RD_SAMPLE is the finer knob within one clock. */
    parameter integer RD_LATENCY = 6
) (
    input  wire                 sys_clk,
    input  wire                 sys_rst,

    /* Read timing, settable at runtime -------------------------------------------------------
     * These were parameters, which meant the one number that can only be found on real hardware
     * could only be changed by resynthesising, placing, routing and reflashing. Every wrong guess
     * cost a full rebuild. As inputs, the Teensy can sweep the whole space in a second and report
     * which settings return correct data. The parameters below remain the reset defaults, so a board
     * that is never configured still behaves exactly as before.
     * -------------------------------------------------------------------------------------- */
    input  wire [7:0]           cfg_rd_latency,
    input  wire [3:0]           cfg_rd_sample,
    input  wire                 cfg_load,       /* pulse to adopt the two values above */

    input  wire                 req_valid,
    output wire                 req_ready,
    input  wire                 req_write,
    input  wire [BA_BITS-1:0]   req_bank,
    input  wire [ROW_BITS-1:0]  req_row,
    input  wire [COL_BITS-1:0]  req_col,
    input  wire [7:0]           req_len,

    input  wire [63:0]          wd_data,
    output reg                  wd_take,

    output reg  [63:0]          rd_data,
    output reg                  rd_valid,
    output wire                 busy,
    output wire                 init_done,

    output reg                  ddr_ck,
    output wire                 ddr_ck_n,
    output reg                  ddr_cke,
    output reg                  ddr_reset_n,
    output reg                  ddr_cs_n,
    output reg                  ddr_ras_n,
    output reg                  ddr_cas_n,
    output reg                  ddr_we_n,
    output reg  [BA_BITS-1:0]   ddr_ba,
    output reg  [15:0]          ddr_a,
    output wire                 ddr_odt,
    output reg  [7:0]           ddr_dq_o,
    output reg                  ddr_dq_oe,
    input  wire [7:0]           ddr_dq_i,
    output reg                  ddr_dqs_o,
    output reg                  ddr_dqs_oe
);

    assign ddr_ck_n = ~ddr_ck;
    assign ddr_odt  = 1'b0;

    /* ---- mode registers, every bit traced to a datasheet figure ---------------------------- */

    /* MR0 fig 46: A1:A0=00 fixed BL8; A3=0 sequential; CL6 encodes as A6,A5,A4,A2 = 0,1,0,0 hence
     * bit 5; A8=0 no DLL reset (there is no DLL); A11:A9=010 write recovery 6 (bit 10), and at this
     * clock the real tWR of 15 ns needs one cycle so any legal code works; A12=0 slow PD exit. */
    localparam [15:0] MR0 = 16'h0420;
    /* MR1 fig 48: A0=1 DISABLES THE DLL, the premise of this controller. A4:A3=00 no additive
     * latency. A5,A1=00 drive RZQ/6. A9,A6,A2=0 Rtt_Nom off, required by p115. A7=0 no write
     * levelling. A12=0 outputs enabled. */
    localparam [15:0] MR1 = 16'h0001;
    /* MR2: A2:A0=001 is CWL6, the only legal value with the DLL off. A10:A9=00 dynamic ODT off,
     * also required. A6=0 ASR off, A7=0 normal SRT. */
    localparam [15:0] MR2 = 16'h0001;
    localparam [15:0] MR3 = 16'h0000;
    localparam integer CWL = 6;

    /* ---- timing: nanoseconds to memory clocks, at elaboration ------------------------------ */
    localparam integer CK_PS = (1_000_000 / (SYS_HZ / 1_000_000)) * CK_DIV;

    function integer cyc_ns(input integer ns);
        begin
            cyc_ns = ((ns * 1000) + CK_PS - 1) / CK_PS;
            if (cyc_ns < 1) cyc_ns = 1;
        end
    endfunction

    localparam integer T_RCD = cyc_ns(14);
    localparam integer T_RP  = cyc_ns(14);
    localparam integer T_RFC = cyc_ns(160);
    localparam integer T_WR  = cyc_ns(15);
    localparam integer T_MRD = 4;
    localparam integer T_MOD = 12;
    localparam integer T_XPR = (T_RFC + cyc_ns(10) > 5) ? T_RFC + cyc_ns(10) : 5;

    /* fabric clocks, so they hold for any CK_DIV */
    localparam integer REFI_SYS = (SYS_HZ / 1_000_000) * 78 / 10;
    localparam integer W_RESET  = (SYS_HZ / 1_000_000) * 200;
    localparam integer W_CKE    = (SYS_HZ / 1_000_000) * 500;

    /* ---- memory clock and phase -------------------------------------------------------------
     * CK rises at phase 0, the edge the DRAM latches commands on, and falls at the midpoint.
     * Command and address are updated on the FALLING edge, giving half a memory period of setup:
     * 10 ns at CK_DIV 2 against a required 170 ps. That margin is why hand wiring works.
     * -------------------------------------------------------------------------------------- */
    localparam integer PH_BITS = (CK_DIV <= 2) ? 1 : $clog2(CK_DIV);
    reg [PH_BITS-1:0] phase;
    wire ck_rise = (phase == 0);
    wire ck_fall = (phase == CK_DIV/2);

    /* Write data beats land a quarter period either side of a strobe edge, so that each beat
     * straddles the edge that captures it. That position only exists if a quarter period is a whole
     * number of fabric clocks, which needs CK_DIV >= 4. At CK_DIV 2 the quarter point collapses onto
     * the edge itself, data changes exactly when the device samples, and writes fail -- verified,
     * not assumed: CK_DIV 2 produces 127 protocol errors and 32 corrupt words in simulation.
     *
     * So 25 MHz is the ceiling for this single-edge design, giving 50 MB/s on eight data lines. The
     * way past it is NOT a faster clock, it is a wider bus: sixteen data lines at 25 MHz is 100 MB/s,
     * which clears the Teensy's 66.5 MB/s link with the edge rate still at a hand-wiring-friendly
     * 25 MHz. Eight more level-shifted wires buy more than doubling the clock would, and are far
     * kinder to flying leads. */
    localparam integer PH_DQ_A = CK_DIV - (CK_DIV/4);
    localparam integer PH_DQ_B = CK_DIV/4;

    initial begin
        if (CK_DIV < 4)
            $display("ddr3_ctrl: CK_DIV=%0d is unusable -- writes cannot be centred on the strobe below 4", CK_DIV);
        if (CK_DIV % 4 != 0)
            $display("ddr3_ctrl: CK_DIV=%0d should be a multiple of 4 so quarter periods are whole clocks", CK_DIV);
    end
    wire dq_slot = (phase == PH_DQ_A) || (phase == PH_DQ_B);

    /* Beats arrive every half memory clock, which is CK_DIV/2 fabric clocks. Structural, so it
     * stays a parameter. */
    localparam integer RD_STEP = (CK_DIV/2) - 1;

    /* The live copies. Loaded from the parameters at reset and replaced whenever cfg_load pulses. */
    reg [7:0] rd_latency_r;
    reg [3:0] rd_sample_r;
    always @(posedge sys_clk) begin
        if (sys_rst) begin
            rd_latency_r <= RD_LATENCY[7:0];
            rd_sample_r  <= RD_SAMPLE[3:0];
        end else if (cfg_load) begin
            rd_latency_r <= cfg_rd_latency;
            rd_sample_r  <= cfg_rd_sample;
        end
    end

    always @(posedge sys_clk) begin
        if (sys_rst) begin
            phase  <= 0;
            ddr_ck <= 1'b0;
        end else begin
            phase <= (phase == CK_DIV-1) ? 0 : phase + 1'b1;
            if (ck_rise) ddr_ck <= 1'b1;
            if (ck_fall) ddr_ck <= 1'b0;
        end
    end

    /* ---- command encoding: CS#, RAS#, CAS#, WE# -------------------------------------------- */
    localparam [3:0] CMD_DESEL = 4'b1111, CMD_NOP = 4'b0111, CMD_ACT = 4'b0011,
                     CMD_READ  = 4'b0101, CMD_WR  = 4'b0100, CMD_PRE = 4'b0010,
                     CMD_REF   = 4'b0001, CMD_MRS = 4'b0000;

    reg [3:0]         cmd;
    reg [15:0]        a_next;
    reg [BA_BITS-1:0] ba_next;

    always @(posedge sys_clk) begin
        if (sys_rst) begin
            {ddr_cs_n, ddr_ras_n, ddr_cas_n, ddr_we_n} <= CMD_DESEL;
            ddr_a  <= 16'h0000;
            ddr_ba <= {BA_BITS{1'b0}};
        end else if (ck_fall) begin
            {ddr_cs_n, ddr_ras_n, ddr_cas_n, ddr_we_n} <= cmd;
            ddr_a  <= a_next;
            ddr_ba <= ba_next;
        end
    end

    /* ---- refresh timer. Free running. Up to 8 postponed, which is the slack that lets a
     * streaming run finish before refresh is serviced. ---------------------------------------- */
    reg [$clog2(REFI_SYS)-1:0] refi_cnt;
    reg [3:0]                  ref_owed;
    reg                        ref_ack;
    wire ref_tick = (refi_cnt == REFI_SYS-1);

    always @(posedge sys_clk) begin
        if (sys_rst) begin
            refi_cnt <= 0;
            ref_owed <= 0;
        end else begin
            refi_cnt <= ref_tick ? 0 : refi_cnt + 1'b1;
            /* Resolved once, here. Two assignments racing in one block would let statement order
             * decide and would drop a refresh whenever the timer ticked on the same cycle one was
             * issued. */
            case ({ref_tick, ref_ack})
                2'b10:   if (ref_owed != 4'd8) ref_owed <= ref_owed + 1'b1;
                2'b01:   if (ref_owed != 4'd0) ref_owed <= ref_owed - 1'b1;
                default: ;
            endcase
        end
    end

    /* ---- sequencer -------------------------------------------------------------------------- */
    localparam [3:0] S_RESET = 0,  S_CKE  = 1,  S_XPR   = 2,  S_MR2 = 3,
                     S_MR3   = 4,  S_MR1  = 5,  S_MR0   = 6,  S_MODW= 7,
                     S_IDLE  = 8,  S_REFW = 9,  S_RCD   = 10, S_RUN = 11,
                     S_DRAIN = 12, S_WREC = 13, S_RPW   = 14, S_PREACT = 15;

    /* ---- open row policy ----------------------------------------------------------------------
     * A row is left OPEN when a run finishes. The next request pays for an ACTIVATE only if it lands
     * somewhere else.
     *
     * Closing the row every time costs ACTIVATE, tRCD, PRECHARGE and tRP on every request, about
     * twelve memory clocks or 0.48 us at 25 MHz. Against eight bursts of payload that is 1.76 us for
     * 64 bytes, which is 36 MB/s where the data alone would be 50 -- and the link then outruns the
     * memory and returns bytes that have not arrived yet. Keeping the row open removes that overhead
     * entirely for sequential access, which is the only pattern that matters when the job is
     * streaming weights.
     *
     * The cost is bookkeeping, and both halves of it are tRP violations if got wrong: a refresh needs
     * every row closed first, and a request to a different row needs a PRECHARGE inserted ahead of its
     * ACTIVATE. The device model checks tRP, so a mistake here fails the test rather than the board.
     * -------------------------------------------------------------------------------------- */
    reg                row_open;
    reg [BA_BITS-1:0]  cur_bank;
    reg [ROW_BITS-1:0] cur_row;

    reg [3:0]          st;
    reg [31:0]         wait_sys;
    reg [9:0]          wait_ck;
    reg [7:0]          left;
    reg [7:0]          rd_due;
    reg [COL_BITS-1:0] col;
    reg                is_wr;

    wire in_init = (st <= S_MODW);
    assign init_done = !in_init;
    assign busy      = (st != S_IDLE);
    /* req_ready means "I am accepting a request ON THIS CYCLE", not "I am available".
     *
     * Requests are only ever latched on a memory clock rising edge, which is one fabric cycle in
     * CK_DIV. An unqualified ready signal is therefore true for CK_DIV-1 cycles on which nothing can
     * actually be accepted, and a requester that drops its valid as soon as it sees ready loses the
     * request three times out of four at CK_DIV 4. That failure is silent and total: the bridge waits
     * forever for data that was never requested. Qualifying with ck_rise makes
     * (req_valid && req_ready) a true acceptance. */
    assign req_ready = (st == S_IDLE) && (ref_owed == 0) && !in_init && ck_rise;

    /* Write pump. A WRITE is issued every four clocks while write latency is six, so burst n+1 is
     * commanded before burst n's data has finished leaving. A single arming slot therefore gets
     * overwritten and half the bytes never reach the device. So the pump arms ONCE, on the first
     * burst of a run, and then streams continuously: eight beats, fetch the next word, eight more,
     * with the strobe toggling unbroken from preamble to postamble. */
    reg [2:0]  wb;          /* beat index within the current burst, 0..7 */
    reg        w_run;       /* a burst stream is live: strobe toggling, bus driven */
    reg [7:0]  w_left;      /* bursts still to emit, including the one in progress */
    reg [63:0] wbuf;
    reg        w_arm;       /* first burst commanded, waiting out write latency */
    reg [3:0]  w_pre;

    /* One strobe edge per data beat, exactly. Toggling for as long as the run is live emits one extra
     * edge after the final beat, because the release slot is half a memory clock after the last beat
     * and a clock edge falls in between. A real device captures on that edge too, so it writes a
     * ninth byte into the next location -- silent corruption of data nobody asked to touch. The
     * device model catches it as a strobe edge with no command outstanding. This flag pairs each
     * beat with its own edge and nothing else. */
    reg        dqs_pend;

    /* Read sampler. Two knobs, together covering any arrival time to fabric-clock resolution:
     *   RD_LATENCY  -- which memory clock the first beat lands in
     *   RD_SAMPLE   -- where inside that clock to look, 0 .. CK_DIV-1
     * The first beat is a ONE SHOT at exactly phase RD_SAMPLE, and every beat after it is counted
     * off in fabric clocks rather than matched against a phase again. Matching a phase pair instead
     * (RD_SAMPLE and RD_SAMPLE + CK_DIV/2) looks equivalent and is not: at CK_DIV 8 both 2 and 6
     * name the same unordered pair, so the sub-clock half of the calibration quietly did not exist
     * and a device half a clock late could not be tuned in at all. */
    reg [7:0]  rd_wait;     /* memory clocks remaining before the first beat */
    reg        rd_run;
    reg        rd_go;       /* the beat train has begun */
    reg [3:0]  rd_sub;      /* fabric clocks until the next beat */
    reg [3:0]  rbeat;
    reg [63:0] rbuf;

    wire rd_first = rd_run && !rd_go && (rd_wait == 0) &&
                    (phase == rd_sample_r[PH_BITS-1:0]);
    wire rd_next  = rd_run && rd_go && (rd_sub == 0);
    wire rd_tick  = rd_first || rd_next;

    always @(posedge sys_clk) begin
        ref_ack  <= 1'b0;
        rd_valid <= 1'b0;
        wd_take  <= 1'b0;

        if (sys_rst) begin
            st <= S_RESET; wait_sys <= W_RESET; wait_ck <= 0;
            cmd <= CMD_DESEL; a_next <= 16'h0000; ba_next <= {BA_BITS{1'b0}};
            ddr_reset_n <= 1'b0; ddr_cke <= 1'b0;
            ddr_dq_oe <= 1'b0; ddr_dq_o <= 8'h00;
            ddr_dqs_oe <= 1'b0; ddr_dqs_o <= 1'b0;
            left <= 0; rd_due <= 0; col <= 0; is_wr <= 1'b0;
            row_open <= 1'b0; cur_bank <= 0; cur_row <= 0;
            wb <= 0; w_run <= 1'b0; w_left <= 0; w_arm <= 1'b0; w_pre <= 0; wbuf <= 64'd0;
            dqs_pend <= 1'b0;
            rd_wait <= 0; rd_run <= 1'b0; rd_go <= 1'b0; rd_sub <= 0;
            rbeat <= 0; rbuf <= 64'd0;
            rd_data <= 64'd0;

        /* The long power-up waits run on the fabric clock so they take the same real time for any
         * CK_DIV. Sequence per datasheet page 126. */
        end else if (st == S_RESET || st == S_CKE) begin
            if (wait_sys != 0) begin
                wait_sys <= wait_sys - 1'b1;
            end else if (st == S_RESET) begin
                /* CKE has been low since reset, covering the 10 ns it must lead RESET# high. */
                ddr_reset_n <= 1'b1;
                st <= S_CKE; wait_sys <= W_CKE;
            end else begin
                ddr_cke <= 1'b1;            /* the clock has been running throughout */
                cmd <= CMD_NOP;
                st <= S_XPR; wait_ck <= T_XPR[9:0];
            end

        end else begin
            if (ck_rise) begin
                cmd <= CMD_NOP;

                case (st)
                S_XPR:  if (wait_ck != 0) wait_ck <= wait_ck - 1'b1;
                        else begin cmd<=CMD_MRS; ba_next<=3'd2; a_next<=MR2; st<=S_MR3; wait_ck<=T_MRD; end
                S_MR3:  if (wait_ck != 0) wait_ck <= wait_ck - 1'b1;
                        else begin cmd<=CMD_MRS; ba_next<=3'd3; a_next<=MR3; st<=S_MR1; wait_ck<=T_MRD; end
                S_MR1:  if (wait_ck != 0) wait_ck <= wait_ck - 1'b1;
                        else begin cmd<=CMD_MRS; ba_next<=3'd1; a_next<=MR1; st<=S_MR0; wait_ck<=T_MRD; end
                S_MR0:  if (wait_ck != 0) wait_ck <= wait_ck - 1'b1;
                        else begin cmd<=CMD_MRS; ba_next<=3'd0; a_next<=MR0; st<=S_MODW; wait_ck<=T_MOD; end
                S_MODW: if (wait_ck != 0) wait_ck <= wait_ck - 1'b1;
                        else begin
                            /* ZQ calibration skipped deliberately: it trims output impedance and
                             * termination, and nothing is terminated at this speed. */
                            cmd <= CMD_PRE; a_next <= 16'h0400;   /* A10 = all banks */
                            row_open <= 1'b0;
                            st <= S_RPW; wait_ck <= T_RP[9:0];
                        end

                S_IDLE:
                    if (ref_owed != 0) begin
                        /* Refresh requires every row closed. Close first, refresh on the next pass. */
                        if (row_open) begin
                            cmd <= CMD_PRE; a_next <= 16'h0400;
                            row_open <= 1'b0;
                            st <= S_RPW; wait_ck <= T_RP[9:0];
                        end else begin
                            cmd <= CMD_REF; ref_ack <= 1'b1;
                            st <= S_REFW; wait_ck <= T_RFC[9:0];
                        end
                    end else if (req_valid) begin
                        col   <= {req_col[COL_BITS-1:3], 3'b000};
                        left  <= req_len;
                        is_wr <= req_write;
                        if (req_write) begin
                            wbuf    <= wd_data;
                            wd_take <= 1'b1;
                            w_left  <= req_len;
                        end
                        if (row_open && req_bank == cur_bank && req_row == cur_row) begin
                            /* Already open: straight to the column commands, no tRCD to wait out.
                             * This is the case that carries every sequential read. */
                            ba_next <= req_bank;
                            st      <= S_RUN; wait_ck <= 0;
                        end else if (row_open) begin
                            /* A different row is open. Close it, then activate the one wanted. */
                            cmd <= CMD_PRE; a_next <= 16'h0400;
                            row_open <= 1'b0;
                            st <= S_PREACT; wait_ck <= T_RP[9:0];
                        end else begin
                            cmd      <= CMD_ACT;
                            ba_next  <= req_bank;
                            a_next   <= {{(16-ROW_BITS){1'b0}}, req_row};
                            row_open <= 1'b1;
                            cur_bank <= req_bank;
                            cur_row  <= req_row;
                            st <= S_RCD; wait_ck <= T_RCD[9:0];
                        end
                    end

                /* The row that was in the way is closed; now activate the one that was asked for. The
                 * bridge is still holding the request, so its address is still valid here. */
                S_PREACT: if (wait_ck != 0) wait_ck <= wait_ck - 1'b1;
                          else begin
                              cmd      <= CMD_ACT;
                              ba_next  <= req_bank;
                              a_next   <= {{(16-ROW_BITS){1'b0}}, req_row};
                              row_open <= 1'b1;
                              cur_bank <= req_bank;
                              cur_row  <= req_row;
                              st <= S_RCD; wait_ck <= T_RCD[9:0];
                          end

                S_REFW: if (wait_ck != 0) wait_ck <= wait_ck - 1'b1; else st <= S_IDLE;
                S_RCD:  if (wait_ck != 0) wait_ck <= wait_ck - 1'b1; else st <= S_RUN;

                /* The streaming heart: one column command every four memory clocks down an open
                 * row, which is what keeps the Teensy's bus fed. */
                S_RUN:
                    if (wait_ck != 0) begin
                        wait_ck <= wait_ck - 1'b1;
                    end else if (left != 0) begin
                        a_next  <= {{(16-COL_BITS){1'b0}}, col} & ~16'h0400;  /* no auto precharge */
                        col     <= col + 8;
                        left    <= left - 1'b1;
                        wait_ck <= 3;                 /* BL8 occupies four clocks */
                        if (is_wr) begin
                            cmd <= CMD_WR;
                            /* Arm on the first burst only. Later bursts just follow the stream; the
                             * pump is already running by the time they are commanded. */
                            if (!w_run && !w_arm) begin
                                w_arm <= 1'b1;
                                w_pre <= CWL - 1;
                            end
                        end else begin
                            cmd    <= CMD_READ;
                            rd_due <= rd_due + 1'b1;
                            if (!rd_run) begin
                                rd_run  <= 1'b1;
                                rd_wait <= rd_latency_r;
                                rd_go   <= 1'b0;
                            end
                        end
                    end else begin
                        st <= S_DRAIN;
                    end

                S_DRAIN:
                    /* Let the last burst finish, then leave the row OPEN. The next request to the same
                     * row costs nothing at all, which is what makes sequential reads hit full rate. */
                    if (is_wr) begin
                        if (!w_run && !w_arm) begin st <= S_WREC; wait_ck <= T_WR[9:0]; end
                    end else if (rd_due == 0) begin
                        st <= S_IDLE;
                    end

                /* tWR after the last write beat. The row stays open here too. */
                S_WREC: if (wait_ck != 0) wait_ck <= wait_ck - 1'b1; else st <= S_IDLE;

                S_RPW:  if (wait_ck != 0) wait_ck <= wait_ck - 1'b1; else st <= S_IDLE;
                default: st <= S_IDLE;
                endcase

                /* countdown from the WRITE command to CWL */
                if (w_arm && w_pre != 0) w_pre <= w_pre - 1'b1;
            end

            /* ---- strobe and data, on quarter-period boundaries ---- */

            /* Start of a run: the strobe is driven low a full memory clock before its first
             * rising edge, where tWPRE asks only for 0.9 tCK, and beat 0 is presented a quarter
             * period early so it straddles that edge. */
            if (w_arm && w_pre == 0 && phase == PH_DQ_A[PH_BITS-1:0] && !w_run) begin
                ddr_dqs_oe <= 1'b1;
                ddr_dqs_o  <= 1'b0;
                ddr_dq_oe  <= 1'b1;
                ddr_dq_o   <= wbuf[7:0];
                wbuf       <= {8'h00, wbuf[63:8]};
                wb         <= 1;
                w_run      <= 1'b1;
                w_arm      <= 1'b0;
                dqs_pend   <= 1'b1;
                /* No fetch here. The word for this burst was taken in S_IDLE; taking again would
                 * advance the producer twice and burst 1 would receive word 2. */
            end else if (w_run && dq_slot) begin
                if (wb != 0) begin
                    /* beats 1..7 of the current burst */
                    ddr_dq_o <= wbuf[7:0];
                    dqs_pend <= 1'b1;
                    wb       <= wb + 1'b1;       /* wraps 7 -> 0 */
                    if (wb == 7) begin
                        /* Last beat of this burst. Load the next word and acknowledge it in the
                         * same slot: exactly one take per word, which is the whole contract. */
                        w_left <= w_left - 1'b1;
                        if (w_left > 1) begin
                            wbuf    <= wd_data;
                            wd_take <= 1'b1;
                        end
                    end else begin
                        wbuf <= {8'h00, wbuf[63:8]};
                    end
                end else if (w_left != 0) begin
                    /* first beat of the next burst, stream unbroken, word already in hand */
                    ddr_dq_o <= wbuf[7:0];
                    wbuf     <= {8'h00, wbuf[63:8]};
                    wb       <= 1;
                    dqs_pend <= 1'b1;
                end else begin
                    /* postamble: the strobe has already settled low, release the bus */
                    ddr_dq_oe  <= 1'b0;
                    ddr_dqs_oe <= 1'b0;
                    w_run      <= 1'b0;
                end
            end

            /* One edge per pending beat, placed in the middle of it. Eight toggles from a low
             * preamble end low again, which is the postamble the datasheet asks for. */
            if (dqs_pend && (ck_rise || ck_fall)) begin
                ddr_dqs_o <= ~ddr_dqs_o;
                dqs_pend  <= 1'b0;
            end

            /* Read sampling at a fixed offset. DQS is ignored on purpose: page 115 permits tDQSCK
             * to exceed tCK with the DLL off, which makes the returned strobe useless as a latch.
             * RD_SAMPLE replaces it and is tuned on hardware. */
            if (rd_run) begin
                if (ck_rise && rd_wait != 0) rd_wait <= rd_wait - 1'b1;

                if (rd_first)            begin rd_go <= 1'b1; rd_sub <= RD_STEP; end
                else if (rd_next)              rd_sub <= RD_STEP;
                else if (rd_go && rd_sub != 0) rd_sub <= rd_sub - 1'b1;

                if (rd_tick) begin
                    rbuf  <= {ddr_dq_i, rbuf[63:8]};
                    rbeat <= rbeat + 1'b1;
                    if (rbeat == 7) begin
                        rd_data  <= {ddr_dq_i, rbuf[63:8]};
                        rd_valid <= 1'b1;
                        rbeat    <= 0;
                        rd_due   <= rd_due - 1'b1;
                        if (rd_due == 1 && left == 0) begin
                            rd_run <= 1'b0;
                            rd_go  <= 1'b0;
                        end
                    end
                end
            end
        end
    end

endmodule

`default_nettype wire
