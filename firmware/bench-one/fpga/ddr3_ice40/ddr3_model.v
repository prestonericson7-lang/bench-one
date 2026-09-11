/* ===========================================================================================
 *  ddr3_model.v -- a DDR3 x8 device that CHECKS its master, for simulation only
 * ===========================================================================================
 *
 *  Not a performance model, not synthesisable. Its job is to fail loudly when the controller does
 *  something a real part would punish silently, because the alternative is finding out with a
 *  soldering iron and 38 flying leads. It enforces:
 *
 *    - CKE low while RESET# is asserted and when it is released
 *    - the mode register order the datasheet mandates: MR2, MR3, MR1, MR0
 *    - MR1[0] = 1, the DLL actually disabled, before any column command is accepted
 *    - CL = 6 and CWL = 6, the only values DLL disable mode allows
 *    - Rtt_Nom and Rtt_WR zero and ODT low, which page 115 also requires
 *    - tRCD after ACTIVATE, tRP after PRECHARGE
 *    - no column command to an unopened row, no ACTIVATE over an already-open one
 *    - no REFRESH with a row open, and the 7.8 us refresh deadline, watched continuously
 *
 *  Write data is captured on the strobe the controller generates, which is the only thing a real
 *  part can do, so a write that forgets DQS stores nothing here either.
 *
 *  RD_DELAY_HALF is how many half clock periods after a READ the first beat appears. The datasheet
 *  says tDQSCK begins AL + CL - 1 cycles after READ, which is 5 cycles or 10 half periods, and then
 *  warns that with the DLL off the delay "could be larger than tCK". Raising this parameter past 10
 *  is how that warning gets tested, and it is the same thing RD_SAMPLE and RD_LATENCY have to
 *  absorb on real hardware.
 * ======================================================================================== */

`default_nettype none
`timescale 1ns / 1ps

/* Every timing below is in NANOSECONDS as a real, compared against $realtime. Written as plain
 * integers they read as whole ns under this timescale, which turned tRCD's 13.75 ns into 13750 ns
 * and made the model reject a controller that was in fact correct. */

module ddr3_model #(
    parameter integer ROW_BITS      = 15,
    parameter integer COL_BITS      = 10,
    parameter integer BA_BITS       = 3,
    parameter integer RD_DELAY_HALF = 10,
    parameter integer CHECK_INIT_US = 0      /* 0 skips the 200/500 us waits so sims finish */
) (
    input  wire                 ck,
    input  wire                 ck_n,
    input  wire                 cke,
    input  wire                 reset_n,
    input  wire                 cs_n,
    input  wire                 ras_n,
    input  wire                 cas_n,
    input  wire                 we_n,
    input  wire [BA_BITS-1:0]   ba,
    input  wire [15:0]          a,
    input  wire                 odt,
    input  wire [7:0]           dq_i,
    input  wire                 dq_oe,
    output reg  [7:0]           dq_o,
    output reg                  dq_oe_m,
    input  wire                 dqs_i,
    input  wire                 dqs_oe
);

    /* A real 2 Gbit x8 part is 8 banks x 32768 rows x 1024 columns, which would be 256 MB of
     * simulator heap. The test only touches a handful of rows, so the array is indexed by a
     * truncated address: bank, the low nine row bits, and the column. */
    localparam integer CELLS = (1 << (BA_BITS + 9 + COL_BITS));
    reg [7:0] mem [0:CELLS-1];

    function [BA_BITS+9+COL_BITS-1:0] cidx;
        input [BA_BITS-1:0]  b;
        input [ROW_BITS-1:0] r;
        input [COL_BITS-1:0] c;
        begin
            cidx = {b, r[8:0], c};
        end
    endfunction

    /* nanoseconds, real, straight from the datasheet */
    localparam real TRCD_NS  = 13.75;
    localparam real TRP_NS   = 13.75;
    localparam real TREFI_NS = 7800.0;

    integer errors    = 0;
    integer writes    = 0;
    integer reads     = 0;
    integer refreshes = 0;
    integer short_bursts = 0;
    integer wcmds = 0;

    task oops;
        input [1023:0] msg;
        begin
            $display("  *** %0t  %0s", $time, msg);
            errors = errors + 1;
        end
    endtask

    /* ---- mode registers as the device sees them ---- */
    reg [15:0] mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    reg        dll_off     = 0;
    reg        initialised = 0;
    integer    mr_order    = 0;

    /* ---- per bank state ---- */
    reg                row_open [0:(1<<BA_BITS)-1];
    reg [ROW_BITS-1:0] open_row [0:(1<<BA_BITS)-1];
    real               t_act    [0:(1<<BA_BITS)-1];
    real               t_pre    [0:(1<<BA_BITS)-1];
    real               t_ref = 0.0;

    integer i;
    initial begin
        for (i = 0; i < (1<<BA_BITS); i = i + 1) begin
            row_open[i] = 1'b0;
            open_row[i] = 0;
            t_act[i]    = 0.0;
            t_pre[i]    = 0.0;
        end
        dq_o    = 8'h00;
        dq_oe_m = 1'b0;
    end

    /* ---- reset and CKE supervision ---- */
    real t_reset_low = 0.0;

    always @(negedge reset_n) begin
        t_reset_low = $realtime;
        if (cke !== 1'b0) oops("CKE must be low while RESET# is asserted");
    end

    always @(posedge reset_n) begin
        if (cke !== 1'b0)
            oops("CKE must still be low when RESET# is released");
        if (CHECK_INIT_US != 0 && (($realtime - t_reset_low) < 200_000.0))
            oops("RESET# was held low for less than 200 us");
    end

    /* ---- command decode ---- */
    wire [3:0] c_cmd = {cs_n, ras_n, cas_n, we_n};
    localparam [3:0] CMD_ACT  = 4'b0011, CMD_READ = 4'b0101, CMD_WR = 4'b0100,
                     CMD_PRE  = 4'b0010, CMD_REF  = 4'b0001, CMD_MRS = 4'b0000;

    /* ---- read return ------------------------------------------------------------------------
     * The controller streams a READ every four clocks while CAS latency is six, so up to two
     * bursts are in flight at once and their beats form one continuous stream. A single-slot
     * return would have each READ overwrite the last and emit nothing but the final burst. So
     * reads queue, and the queue is drained one beat per half clock.
     * -------------------------------------------------------------------------------------- */
    localparam integer RDQ = 8;
    reg [63:0] rdq_data [0:RDQ-1];
    integer    rdq_wr   = 0;          /* next slot to fill */
    integer    rdq_rd   = 0;          /* slot being emitted */
    integer    rdq_cnt  = 0;
    integer    rd_beat  = 0;
    integer    hclk     = 0;          /* half clocks since reset */
    integer    rd_due   = -1;

    /* ---- write landing zone ------------------------------------------------------------------
     * A WRITE is commanded every four clocks while write latency is six, so the next command
     * arrives while the previous burst's beats are still being captured. Holding one address and
     * resetting the beat counter on each command therefore scatters each burst's tail into the next
     * burst's first bytes. The addresses queue instead, and one is retired per eight strobe edges.
     * -------------------------------------------------------------------------------------- */
    localparam integer WRQ = 8;
    reg [BA_BITS-1:0]  wq_bank [0:WRQ-1];
    reg [ROW_BITS-1:0] wq_row  [0:WRQ-1];
    reg [COL_BITS-1:0] wq_col  [0:WRQ-1];
    integer            wq_wr   = 0;
    integer            wq_rd   = 0;
    integer            wq_cnt  = 0;
    reg [3:0]          wr_beat = 0;

    /* One process for both edges. The command decode and the beat emission used to live in
     * separate always blocks that both triggered on the rising edge, one incrementing hclk and the
     * other reading it, which is a race the simulator is free to resolve either way. Merging them
     * makes the order explicit: decode, then emit. */
    always @(ck) begin
        hclk = hclk + 1;
        if (ck === 1'b1 && reset_n && cke) begin
            case (c_cmd)
            CMD_MRS: begin
                mr_order = mr_order + 1;
                case (ba)
                3'd2: begin
                    mr2 = a;
                    if (mr_order != 1) oops("MR2 must be the first mode register written");
                    if (a[2:0] !== 3'b001)
                        oops("CWL must be 6, MR2[2:0] = 001, in DLL disable mode");
                    if (a[10:9] !== 2'b00)
                        oops("Rtt_WR must be disabled, MR2[10:9] = 00, with the DLL off");
                end
                3'd3: begin
                    mr3 = a;
                    if (mr_order != 2) oops("MR3 must be written second");
                end
                3'd1: begin
                    mr1 = a;
                    if (mr_order != 3) oops("MR1 must be written third");
                    dll_off = a[0];
                    if (!a[0])             oops("MR1[0] is 0: the DLL was left enabled");
                    if (a[4:3] !== 2'b00)  oops("additive latency must be 0");
                    if (a[9] || a[6] || a[2]) oops("Rtt_Nom must be disabled with the DLL off");
                    if (a[7])              oops("write levelling is not used with the DLL off");
                end
                3'd0: begin
                    mr0 = a;
                    if (mr_order != 4) oops("MR0 must be written last");
                    if (a[1:0] !== 2'b00) oops("burst length must be fixed BL8, MR0[1:0] = 00");
                    /* CL is encoded in A6, A5, A4, A2; CL6 is 0, 1, 0, 0 */
                    if (!(a[6] === 1'b0 && a[5] === 1'b1 && a[4] === 1'b0 && a[2] === 1'b0))
                        oops("CL must be 6 in DLL disable mode");
                    initialised = 1'b1;
                end
                default: oops("mode register select to a bank above 3");
                endcase
            end

            CMD_ACT: begin
                if (!initialised)  oops("ACTIVATE before initialisation completed");
                if (row_open[ba])  oops("ACTIVATE to a bank that already has a row open");
                if (t_pre[ba] != 0.0 && (($realtime - t_pre[ba]) < TRP_NS))
                    oops("tRP violated: ACTIVATE too soon after PRECHARGE");
                row_open[ba] = 1'b1;
                open_row[ba] = a[ROW_BITS-1:0];
                t_act[ba]    = $realtime;
            end

            CMD_READ: begin
                if (!initialised)   oops("READ before initialisation completed");
                if (!dll_off)       oops("READ while the DLL is still enabled");
                if (odt !== 1'b0)   oops("ODT must be held low; unsupported with the DLL off");
                if (!row_open[ba])  oops("READ to a bank with no open row");
                else if (($realtime - t_act[ba]) < TRCD_NS)
                    oops("tRCD violated: READ too soon after ACTIVATE");
                reads = reads + 1;
                if (rdq_cnt >= RDQ) oops("more reads in flight than the model can hold");
                rdq_data[rdq_wr] = {mem[cidx(ba, open_row[ba], a[COL_BITS-1:0] + 7)],
                                    mem[cidx(ba, open_row[ba], a[COL_BITS-1:0] + 6)],
                                    mem[cidx(ba, open_row[ba], a[COL_BITS-1:0] + 5)],
                                    mem[cidx(ba, open_row[ba], a[COL_BITS-1:0] + 4)],
                                    mem[cidx(ba, open_row[ba], a[COL_BITS-1:0] + 3)],
                                    mem[cidx(ba, open_row[ba], a[COL_BITS-1:0] + 2)],
                                    mem[cidx(ba, open_row[ba], a[COL_BITS-1:0] + 1)],
                                    mem[cidx(ba, open_row[ba], a[COL_BITS-1:0] + 0)]};
                /* A burst due before the one ahead of it has finished would mean the controller
                 * issued column commands closer than BL8 allows, so the later time wins and the
                 * stream stays contiguous. */
                if (rdq_cnt == 0 || rd_due < 0)
                    rd_due = hclk + RD_DELAY_HALF;
                rdq_wr  = (rdq_wr + 1) % RDQ;
                rdq_cnt = rdq_cnt + 1;
            end

            CMD_WR: begin
                if (!initialised)   oops("WRITE before initialisation completed");
                if (!dll_off)       oops("WRITE while the DLL is still enabled");
                if (odt !== 1'b0)   oops("ODT must be held low; unsupported with the DLL off");
                if (!row_open[ba])  oops("WRITE to a bank with no open row");
                else if (($realtime - t_act[ba]) < TRCD_NS)
                    oops("tRCD violated: WRITE too soon after ACTIVATE");
                if (wq_cnt >= WRQ) oops("more writes in flight than the model can hold");
                wq_bank[wq_wr] = ba;
                wq_row[wq_wr]  = open_row[ba];
                wq_col[wq_wr]  = a[COL_BITS-1:0];
                wcmds          = wcmds + 1;
                wq_wr          = (wq_wr + 1) % WRQ;
                wq_cnt         = wq_cnt + 1;
            end

            CMD_PRE: begin
                if (a[10]) begin
                    for (i = 0; i < (1<<BA_BITS); i = i + 1) begin
                        if (row_open[i]) t_pre[i] = $realtime;
                        row_open[i] = 1'b0;
                    end
                end else begin
                    if (row_open[ba]) t_pre[ba] = $realtime;
                    row_open[ba] = 1'b0;
                end
            end

            CMD_REF: begin
                if (!initialised) oops("REFRESH before initialisation completed");
                for (i = 0; i < (1<<BA_BITS); i = i + 1)
                    if (row_open[i]) oops("REFRESH issued with a row still open");
                t_ref     = $realtime;
                refreshes = refreshes + 1;
            end

            default: ;   /* NOP, DESELECT */
            endcase
        end

        /* ---- read beats: one per half clock, drained from the queue ---- */
        if (rdq_cnt > 0 && rd_due >= 0 && hclk >= rd_due) begin
            dq_o    = rdq_data[rdq_rd][8*rd_beat +: 8];
            dq_oe_m = 1'b1;
            rd_beat = rd_beat + 1;
            if (rd_beat == 8) begin
                rd_beat = 0;
                rdq_rd  = (rdq_rd + 1) % RDQ;
                rdq_cnt = rdq_cnt - 1;
                rd_due  = (rdq_cnt > 0) ? (hclk + 1) : -1;   /* next burst follows with no gap */
            end
        end else begin
            dq_oe_m = 1'b0;
        end
    end



    /* ---- write capture, on both strobe edges, exactly as the device must ---- */
    /* The write preamble drives the strobe from high impedance down to 0 roughly a clock before its
     * first rising edge. That downward transition is a negedge like any other, and a model that
     * captures on every edge counts it as a data beat, reporting one spurious byte per burst run and
     * blaming the controller for it. Real silicon gates its capture window open on the first RISING
     * edge after the preamble, so this does too: posedges always count, negedges only once the run
     * has properly begun. */
    reg dqs_started = 0;

    always @(negedge dqs_oe) begin
        dqs_started = 0;
        /* The bus was released mid-burst: fewer than eight strobe edges arrived for the word the
         * device was still expecting. One missing byte at the end of a transfer looks like a buffer
         * bug and is actually the controller letting go one edge early. */
        if (wr_beat != 0) begin
            $display("  *** %0t strobe released after only %0d of 8 beats", $realtime, wr_beat);
            short_bursts = short_bursts + 1;
            wr_beat = 0;
        end
    end

    task capture;
        begin
            if (!dq_oe)
                oops("strobe edge during a write with the data bus not driven");
            else if (wq_cnt == 0)
                oops("strobe edge with no WRITE command outstanding");
            else begin
                mem[cidx(wq_bank[wq_rd], wq_row[wq_rd], wq_col[wq_rd] + wr_beat)] = dq_i;
                writes  = writes + 1;
                wr_beat = wr_beat + 1'b1;
                if (wr_beat == 8) begin
                    wr_beat = 0;
                    wq_rd   = (wq_rd + 1) % WRQ;
                    wq_cnt  = wq_cnt - 1;
                end
            end
        end
    endtask

    always @(posedge dqs_i) begin
        if (dqs_oe && reset_n) begin
            dqs_started = 1;
            capture;
        end
    end

    always @(negedge dqs_i) begin
        if (dqs_oe && reset_n && dqs_started) capture;
    end

    /* ---- the refresh deadline, using the rule the datasheet actually states ------------------
     * A gap longer than 7.8 us is NOT a violation. DDR3 explicitly permits up to eight REFRESH
     * commands to be postponed, which is what makes a 20 us streaming burst legal at all. The real
     * requirement is on the running average: 8192 commands per 64 ms, with the accumulated debt never
     * exceeding eight. Checking the gap instead of the debt rejects a controller that is behaving
     * exactly as designed, which is how this model first libelled its own master.
     * -------------------------------------------------------------------------------------- */
    real t_init = 0.0;
    real debt;
    always @(posedge initialised) t_init = $realtime;

    always begin
        #500;
        if (initialised && t_init != 0.0) begin
            debt = (($realtime - t_init) / TREFI_NS) - refreshes;
            if (debt > 8.0) begin
                oops("refresh debt above 8: the controller is falling behind the array");
                t_init = $realtime;     /* re-baseline so this reports once per lapse */
                refreshes = 0;
            end
        end
    end

    task report;
        begin
            $display("  model: %0d bytes written, %0d read bursts, %0d refreshes, %0d errors",
                     writes, reads, refreshes, errors);
        end
    endtask

endmodule

`default_nettype wire
