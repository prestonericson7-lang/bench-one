/* ===========================================================================================
 *  ddr3_bridge.v -- join the Teensy's QSPI port to the DDR3 controller, and nothing more
 * ===========================================================================================
 *
 *  This module is why the measurements can claim to be a Teensy's. It moves bytes between a buffer
 *  and DRAM and performs no arithmetic: no unpacking, no accumulation, no reordering. A byte written
 *  at address N reads back at address N, unchanged.
 *
 *  RATE MATCHING, WHICH IS THE WHOLE DESIGN
 *  ----------------------------------------
 *  FlexSPI cannot be stalled. Once the dummy cycles end it clocks data out relentlessly, so DRAM has
 *  to supply bytes at least as fast as the Teensy takes them.
 *
 *      eight data lines, 25 MHz memory clock, BL8 every four clocks  ->  50 MB/s
 *      FlexSPI on four lines at 100 MHz                              ->  50 MB/s
 *      FlexSPI on four lines at 133 MHz                              ->  66.5 MB/s
 *
 *  Stage one therefore runs the Teensy's bus at 100 MHz, not its 133 MHz maximum: a 133 MHz link
 *  against a 50 MB/s memory reads ahead of the data and returns rubbish. Going to 133 MHz belongs
 *  with a sixteen-line data path, which doubles DRAM throughput while leaving the edge rate at a
 *  hand-wiring-friendly 25 MHz.
 *
 *  THREE THINGS THE END-TO-END SIMULATION FORCED, EACH OF WHICH WAS A REAL BUG
 *  --------------------------------------------------------------------------
 *  1. req_valid is HELD until the controller takes it, not pulsed for one cycle. The controller only
 *     accepts in its idle state with no refresh owed, and a refresh falling due on exactly the cycle
 *     the pulse appeared swallowed the request and wedged the bridge forever. A held handshake cannot
 *     lose a request no matter when refresh interrupts.
 *
 *  2. A read fills the WHOLE buffer, crossing row boundaries as needed. A DDR3 row holds 1 KB for a
 *     x8 device, so a request starting near the end of a row could only fetch a handful of bytes --
 *     yet the Teensy's AHB buffer will happily clock out 512 of them, and the remainder would have
 *     been whatever the buffer held from the previous transaction. Stale data that looks like real
 *     data is the worst possible failure, so the fetch loops until the buffer is full.
 *
 *  3. Byte assembly is a plain counted shift, not a conditional mess keyed off rd_valid. The buffer's
 *     read data is registered, so a byte fetched at step k only appears at step k+1, and writing that
 *     off by one silently shifts every write by one byte.
 *
 *  ADDRESS MAP
 *  -----------
 *  Eight data lines reach exactly ONE chip. A DIMM rank is eight x8 devices sharing address and
 *  command, each supplying eight of sixty-four data bits, so eight wires see one device:
 *  8 banks x 32768 rows x 1 KB = 256 MB. The other 1.75 GB of a 4 GB stick is behind the other
 *  fifty-six lines. 256 MB still nearly fills the Teensy's 240 MB window exactly.
 *
 *      addr[2:0]    byte within a burst
 *      addr[9:3]    burst within the row, 128 of them
 *      addr[12:10]  bank
 *      addr[27:13]  row
 * ======================================================================================== */

`default_nettype none
`timescale 1ns / 1ps

module ddr3_bridge #(
    parameter integer ROW_BITS = 15,
    parameter integer COL_BITS = 10,
    parameter integer BA_BITS  = 3,
    parameter integer BUF_BITS = 10,         /* 1 KB, exactly one DDR3 row for a x8 device */

    /* Memory clock divider, needed here only to size the request limit below. */
    parameter integer SYS_HZ   = 100_000_000,
    parameter integer CK_DIV   = 4,

    /* Bursts fetched per DRAM request during a read. This is a RESPONSIVENESS limit, not a bandwidth
     * one, and getting it wrong is expensive in a way that is easy to miss.
     *
     * Filling the whole 1 KB buffer on every read takes 128 bursts, which is 20.5 us at a 25 MHz
     * memory clock, and the bridge can accept nothing for all of it. A Teensy reading 32-byte cache
     * lines issues a transaction roughly every microsecond, so essentially every request after the
     * first arrives while busy and is served late, with whatever the buffer still held.
     *
     * Sixteen bursts is 128 bytes and about 2.96 us including the CAS pipeline and the store. Eight
     * was tried and is worse, not better: the per-chunk overhead does not shrink with the chunk, so at
     * eight bursts it is 24% of the time and the effective rate falls to 36 MB/s, below the link.
     * Sixteen amortises it to 43 MB/s, and the open-row policy in the controller is what makes even
     * that possible -- without it every chunk pays a fresh ACTIVATE and PRECHARGE.
     *
     * The fetch then continues chunk by chunk for as long as the Teensy keeps clocking and stops when
     * chip select rises, so the amount fetched matches the length of the actual transaction without
     * anyone having to know it in advance. */
    /* RD_CHUNK is DERIVED, not given: see below. */

    /* How far ahead a read fetches, in bursts. This is what decides whether the bridge is free when
     * the NEXT transaction arrives, and it was the root cause of nearly every deferral.
     *
     * Fetching until the buffer is full means 128 bursts, and since the fetch only stops when chip
     * select rises it always overruns past the end of the transaction -- so the bridge is still busy
     * when the next one starts, and that request gets deferred. A deferred write is the worst case:
     * its data sits in a buffer that the following transaction is free to overwrite.
     *
     * 40 bursts is 320 bytes, comfortably ahead of the 256-byte transactions the AHB buffer issues,
     * and the fetch then finishes on its own about half a microsecond after the data phase ends. Raise
     * it only alongside the AHB buffer size, and never above the 128 bursts the buffer can hold. */
    parameter integer RD_AHEAD = 40
) (
    input  wire                 sys_clk,
    input  wire                 sys_rst,

    /* ---- from the QSPI slave, which lives in the sck domain ---- */
    input  wire                 q_start,
    input  wire                 q_is_write,
    input  wire [31:0]          q_addr,
    input  wire [31:0]          q_win_base,
    input  wire                 q_cs_n,
    input  wire [BUF_BITS-1:0]  q_wr_count,

    /* ---- buffer, DRAM side. Two separate memories: see the note in ddr3_top.v. ---- */
    output reg  [BUF_BITS-1:0]  buf_addr,
    output reg  [7:0]           buf_wdata,
    output reg                  buf_we,
    input  wire [7:0]           buf_rdata,

    /* ---- to the DDR3 controller ---- */
    output reg                  req_valid,
    input  wire                 req_ready,
    output reg                  req_write,
    output reg  [BA_BITS-1:0]   req_bank,
    output reg  [ROW_BITS-1:0]  req_row,
    output reg  [COL_BITS-1:0]  req_col,
    output reg  [7:0]           req_len,
    output reg  [63:0]          wd_data,
    input  wire                 wd_take,
    input  wire [63:0]          rd_data,
    input  wire                 rd_valid,
    input  wire                 init_done,

    output wire                 idle,
    output reg                  err_overrun,  /* a burst arrived before the last one was stored */
    output reg                  err_deferred, /* a request arrived while busy and had to wait */
    output reg                  err_align     /* an unaligned WRITE was requested; see below */
);

    /* Longest single request, in bursts, derived rather than chosen. This is a REFRESH budget, not a
     * buffer limit: the controller services refresh only between requests and may postpone at most
     * eight, so a request must not last longer than the refresh interval times the allowance. The
     * budget is taken as FOUR intervals, half the permitted debt, leaving room for the controller to
     * catch up afterwards.
     *
     *     one burst = 4 memory clocks,  interval = 7.8 us
     *     CK_DIV  4 -> 25.00 MHz -> 160 ns/burst -> 195 bursts in 4 intervals -> capped at 128
     *     CK_DIV  8 -> 12.50 MHz -> 320 ns/burst ->  97 bursts
     *     CK_DIV 16 ->  6.25 MHz -> 640 ns/burst ->  48 bursts
     *
     * Hard-coding 128 was safe at 25 MHz and silently destroys data at 6.25 MHz, where a full-length
     * request runs 82 us and owes more than ten refreshes against an allowance of eight. That is the
     * kind of failure that shows up as one wrong byte an hour.
     */
    localparam integer BURST_PS  = (1_000_000 / (SYS_HZ / 1_000_000)) * CK_DIV * 4;
    localparam integer BUDGET_PS = 4 * 7_800_000;
    localparam integer CAP       = BUDGET_PS / BURST_PS;
    localparam integer REQ_MAX   = (CAP > 128) ? 128 : CAP;

    /* Bursts per DRAM request during a read, derived from the memory clock.
     *
     * The dummy window has to cover the first chunk reaching the buffer, and the LUT's dummy field
     * stops at 255 cycles. A chunk of sixteen bursts is 2.56 us at 25 MHz and fits; the same sixteen
     * at 6.25 MHz is 10.2 us, which is 506 cycles at 49.5 MHz and cannot be expressed. Scaling with
     * the clock keeps the chunk near 2.5 us whatever the memory is doing, so this cannot be set to a
     * value the link is unable to wait for. */
    localparam integer CHUNK_RAW = 64 / CK_DIV;
    localparam integer RD_CHUNK  = (CHUNK_RAW < 4) ? 4 : CHUNK_RAW;

    /* ---- cross the two control signals in -----------------------------------------------------
     * Three flops, then an edge detector. q_addr is not synchronised and does not need to be: it is
     * written on the same sck edge that raises q_start and held for the whole transaction, so by the
     * time the edge has propagated the address has been stable for several sys_clk periods.
     * -------------------------------------------------------------------------------------- */
    reg [2:0] st_sync, cs_sync;
    always @(posedge sys_clk) begin
        if (sys_rst) begin
            st_sync <= 3'b000;
            cs_sync <= 3'b111;
        end else begin
            st_sync <= {st_sync[1:0], q_start};
            cs_sync <= {cs_sync[1:0], q_cs_n};
        end
    end
    wire start_edge = (st_sync[2:1] == 2'b01);
    wire cs_rise    = (cs_sync[2:1] == 2'b01);

    /* A request that arrives while the bridge is busy is LATCHED, not dropped.
     *
     * Dropping it was silent and total: the Teensy clocked out its dummy cycles and then read whatever
     * the buffer happened to hold from the previous transaction, which is stale data wearing the shape
     * of real data. Latching at least serves it, a little late, and err_deferred records that it
     * happened so bring-up can see it on an LED rather than inferring it from bad numbers.
     *
     * Being late is still wrong if the dummy window expires first, so this is a safety net and not a
     * licence. The window has to cover the worst case: with cache-line-sized writes, a 32-byte drain
     * is about 640 ns at a 12.5 MHz memory clock against a dummy window of 2.4 us, which is
     * comfortable. A 256-byte write takes 5.1 us to drain and no dummy count covers it, so a large
     * write followed immediately by a read needs a gap the driver has to provide. */
    reg                pend;
    reg                pend_write;
    reg [31:0]         pend_addr;
    reg [BUF_BITS-1:0] pend_count;   /* bytes the deferred write delivered, latched at chip select */
    /* Has the deferred request's transaction actually ENDED?
     *
     * A pending write can become acceptable while its own transaction is still running, if the bridge
     * happens to fall idle mid-transaction. Taking the shortcut then uses a byte count that has not
     * been captured yet -- zero -- so the write issues no bursts at all and the data is silently never
     * stored. A read of that address afterwards returns an undefined buffer, which looks like a read
     * fault and is a write that never happened. */
    reg                pend_done;

    /* Sticky: the read transaction this fetch belongs to has ended.
     *
     * Testing the chip-select LEVEL at a chunk boundary does not work. The gap between two
     * transactions is about 48 ns, and a chunk is 1.28 us, so the high period falls entirely between
     * two tests and is never seen. The fetch then runs to the full buffer -- 128 bursts, some 20 us --
     * and the next request waits far beyond any dummy window that could cover it. The symptom is a
     * read returning the PREVIOUS transaction's data in full, which looks like an addressing fault.
     *
     * The edge detector cannot miss that pulse, because 48 ns is several sys_clk periods, so the edge
     * is latched here and the chunk loop tests the latch. */
    reg                rd_ended;

    /* Which request the idle state is about to service, computed once.
     *
     * Selecting inline with (start_edge && !pend) three separate times was a hazard: if a request
     * arrived while the bridge was idle AND an older one was still pending, every one of those tests
     * chose the OLD request while the same cycle cleared pend -- servicing the stale address and
     * discarding the new request entirely. Deciding once removes the possibility of the three tests
     * disagreeing. */
    wire        sel_pend  = pend;
    wire [31:0] sel_addr  = sel_pend ? pend_addr  : (q_addr + q_win_base);
    wire        sel_write = sel_pend ? pend_write : q_is_write;

    localparam [3:0] B_IDLE  = 0,
                     B_RD_REQ = 1, B_RD_WAIT = 2,
                     B_WR_WAIT= 3, B_WR_LOAD = 4, B_WR_REQ = 5, B_WR_HOLD = 6;

    reg [3:0]          bst;
    reg [31:0]         addr;        /* effective address of the next burst to transfer */
    reg [BUF_BITS-1:0] fill;        /* next buffer byte to write (reads) */
    reg [8:0]          need;        /* bursts still to request for this transaction */
    reg [7:0]          got;         /* bursts still expected from the current request */
    /* Bytes of the first burst to discard, so that buffer[0] really is the requested address.
     *
     * DDR3 transfers whole eight-byte bursts, so a request for address N arrives as the burst
     * containing N. Filling the buffer from the burst boundary puts the wrong byte at buffer[0] and
     * displaces the entire transfer by up to seven bytes -- data that is present, plausible, and in
     * the wrong place, which is the hardest kind of wrong to notice. Discarding the leading bytes
     * fixes reads completely.
     *
     * Writes cannot be fixed the same way. Writing a partial burst needs the data mask, and DM is
     * tied low here so every byte of every burst is committed. An unaligned write is therefore
     * refused rather than misplaced: err_align latches and the transfer is aligned down, which is
     * visible instead of silent. In practice a Teensy never does this -- writes reach the bridge as
     * cache-line writebacks, which are 32-byte aligned. */
    reg [2:0]          skip;
    reg [63:0]         racc;        /* word being unpacked into the buffer */
    reg [3:0]          rcnt;        /* bytes left to store from racc */
    reg [63:0]         wacc;        /* word being assembled from the buffer */
    reg [3:0]          wcnt;        /* bytes still to fetch into wacc */
    reg [7:0]          wleft;       /* bursts still to hand the controller */
    /* ONE authoritative pointer: wnext is the index of the next byte to consume. buf_addr is derived
     * from it inside the load loop and never used as the pointer itself.
     *
     * Every earlier arrangement failed the same way. The buffer's read is registered, so the byte for
     * an address appears a cycle later, and any scheme where the address register is shared between a
     * loop and a wait state drifts: while B_WR_REQ waited for the controller, buf_addr held its value
     * and the registered read quietly advanced one byte past the one wanted. The symptom was eight
     * correct bytes followed by a duplicate -- which reads like a buffer bug and is a pipeline bug.
     *
     * The fix is uniformity: one ten-cycle load, two cycles to fill the read pipeline and eight to
     * shift, used identically for the first word and every word after it. No special cases. */
    reg [BUF_BITS-1:0] wnext;

    /* wacc holds a complete word that has not yet been handed over.
     *
     * The controller latches wd_data on the SAME cycle it pulses wd_take, so the next word has to be
     * presented BEFORE the take, not in response to it. Updating wd_data on the take handed the
     * controller the previous word every time, and since the first word is correct that shows up as
     * every burst writing word 0 -- eight right bytes followed by the same eight bytes forever, which
     * looks like a buffer addressing fault and is not one.
     *
     * This flag also absorbs the take that the controller emits when it ACCEPTS the request. That one
     * acknowledges word 0, which was already presented, so it must not be mistaken for a request for
     * the next. */
    reg                wr_ready;
    reg                wfirst;   /* this load is the first word, so it must raise the request */

    assign idle = (bst == B_IDLE);

    /* Split the flat address. Registered values, so this is a decode of `addr` and not of a live
     * input; that matters because addr advances as the fetch walks across rows. */
    wire [BA_BITS-1:0]  a_bank = addr[12:10];
    wire [ROW_BITS-1:0] a_row  = addr[27:13];
    wire [6:0]          a_brst = addr[9:3];
    wire [8:0]          to_row_end = 9'd128 - {2'b00, a_brst};
    wire [8:0]          this_len   = (need < to_row_end) ? need : to_row_end;
    /* One chunk: whichever is smallest of what is left, what fits in this row, and the chunk size. */
    wire [8:0]          chunk      = (this_len < RD_CHUNK) ? this_len : RD_CHUNK[8:0];

    always @(posedge sys_clk) begin
        buf_we <= 1'b0;

        if (sys_rst) begin
            bst <= B_IDLE; addr <= 32'd0; fill <= 0; need <= 0; got <= 0;
            racc <= 64'd0; rcnt <= 0; wacc <= 64'd0; wcnt <= 0; wleft <= 0;
            buf_addr <= 0; buf_wdata <= 8'h00;
            req_valid <= 1'b0; req_write <= 1'b0;
            req_bank <= 0; req_row <= 0; req_col <= 0; req_len <= 0;
            wd_data <= 64'd0; err_overrun <= 1'b0; err_deferred <= 1'b0; err_align <= 1'b0;
            pend <= 1'b0; pend_write <= 1'b0; pend_addr <= 32'd0; pend_count <= 0;
            pend_done <= 1'b0;
            rd_ended <= 1'b0;
        end else begin

            /* --------- storing read data into the buffer, independent of the state machine ------
             * A burst arrives every four memory clocks, sixteen fabric clocks at CK_DIV 4, and this
             * takes eight. If that ever stops being true the overrun flag catches it rather than
             * quietly interleaving two bursts. */
            if (rd_valid) begin
                if (rcnt != 0) err_overrun <= 1'b1;
                racc <= rd_data;
                rcnt <= 8;
            end else if (rcnt != 0) begin
                if (skip != 0) begin
                    /* discard a leading byte of the first burst */
                    skip <= skip - 1'b1;
                end else begin
                    buf_addr  <= fill;
                    buf_wdata <= racc[7:0];
                    buf_we    <= 1'b1;
                    fill      <= fill + 1'b1;
                end
                racc <= {8'h00, racc[63:8]};
                rcnt <= rcnt - 1'b1;
            end

            /* Capture a request whenever one arrives. If the bridge is mid-transfer it is held here
             * until idle instead of being thrown away. */
            if (start_edge && init_done) begin
                if (bst == B_IDLE) begin
                    pend <= 1'b0;
                end else if (pend) begin
                    /* A second request while one is already waiting. The slot can only hold one, and
                     * the older one's data may already be gone, so this is recorded as an outright
                     * loss rather than silently overwritten. Overwriting was the cascade that made
                     * every transaction after the first deferral return stale data. */
                    err_deferred <= 1'b1;
                end else begin
                    pend         <= 1'b1;
                    pend_write   <= q_is_write;
                    pend_addr    <= q_addr + q_win_base;
                    pend_done    <= 1'b0;
                    err_deferred <= 1'b1;
                end
            end

            /* The byte count of a deferred write is only final when its transaction ends, so it is
             * captured here rather than at defer time. Without this the write is later accepted and
             * then waits for a chip-select rise belonging to some LATER transaction, which poisons
             * every request after it. */
            if (cs_rise && pend) begin
                pend_count <= q_wr_count;
                pend_done  <= 1'b1;
            end

            /* Latch the end of the transaction a read fetch belongs to. */
            if (cs_rise && (bst == B_RD_REQ || bst == B_RD_WAIT)) rd_ended <= 1'b1;

            case (bst)
            B_IDLE:
                if ((start_edge || pend) && init_done) begin
                    addr  <= sel_addr;
                    fill  <= 0;
                    rcnt  <= 0;
                    /* If a NEW request arrived this very cycle while an older one was pending, the old
                     * one is being serviced now and the new one takes the slot rather than being lost. */
                    if (start_edge && sel_pend) begin
                        pend       <= 1'b1;
                        pend_write <= q_is_write;
                        pend_addr  <= q_addr + q_win_base;
                        pend_done  <= 1'b0;
                    end else begin
                        pend <= 1'b0;
                    end
                    if (sel_write) begin
                        /* Writes must land on a burst boundary; see the note on `skip`. */
                        if (sel_addr & 32'd7) err_align <= 1'b1;
                        skip <= 3'd0;
                        if (sel_pend && pend_done) begin
                            /* Deferred: the transaction has already ended and its count was latched
                             * at chip select, so there is nothing to wait for. Waiting here is what
                             * made a deferred write hang until an unrelated transaction ended. */
                            wleft  <= pend_count[BUF_BITS-1:3];
                            wnext  <= 0;
                            wfirst <= 1'b1;
                            if (pend_count[BUF_BITS-1:3] == 0) bst <= B_IDLE;
                            else begin
                                buf_addr <= 0;
                                wcnt     <= 10;
                                bst      <= B_WR_LOAD;
                            end
                        end else begin
                            bst <= B_WR_WAIT;
                        end
                    end else begin
                        /* Reads discard the leading bytes of the first burst, so buffer[0] is the
                         * byte that was actually asked for. */
                        skip <= sel_addr[2:0];
                        /* Fill the entire buffer, whatever the transaction turns out to be. The
                         * Teensy's AHB buffer can ask for 512 bytes and we have no way to know in
                         * advance, so the only safe answer is to have all 1 KB ready. */
                        need     <= RD_AHEAD[8:0];
                        rd_ended <= 1'b0;
                        bst      <= B_RD_REQ;
                    end
                end

            /* ---- read: fetch in chunks for as long as the Teensy is still clocking ---- */
            B_RD_REQ: begin
                if (need == 0 || rd_ended) begin
                    /* Either the buffer is full or the transaction has ended. Nothing is wasted by
                     * stopping early: the Teensy only reads what it asked for, and stopping promptly
                     * is what keeps the next request from being deferred past its dummy window. */
                    bst <= B_IDLE;
                end else begin
                    req_write <= 1'b0;
                    req_bank  <= a_bank;
                    req_row   <= a_row;
                    req_col   <= {a_brst, 3'b000};
                    req_len   <= chunk[7:0];
                    req_valid <= 1'b1;            /* HELD until taken; see the header */
                    if (req_valid && req_ready) begin
                        req_valid <= 1'b0;
                        got       <= chunk[7:0];
                        need      <= need - chunk;
                        addr      <= addr + (chunk << 3);
                        bst       <= B_RD_WAIT;
                    end
                end
            end

            B_RD_WAIT: begin
                if (rd_valid && got != 0) got <= got - 1'b1;
                /* Wait for the last word to be stored as well as received, or the next request would
                 * overwrite racc mid-store. */
                if (got == 0 && rcnt == 0 && !rd_valid) bst <= B_RD_REQ;
            end

            /* ---- write: the Teensy has to finish delivering before anything can be sent ---- */
            B_WR_WAIT:
                if (cs_rise) begin
                    /* q_wr_count is the QSPI slave's buffer pointer, which stops advancing when the
                     * clock stops, so it is stable and equal to the byte count by the time chip
                     * select has risen. Whole bursts only: a partial tail is not written, because
                     * DDR3 has no byte enables wired here. */
                    wleft <= q_wr_count[BUF_BITS-1:3];
                    if (q_wr_count[BUF_BITS-1:3] == 0) bst <= B_IDLE;
                    else begin
                        wnext  <= 0;
                        wcnt   <= 10;
                        wfirst <= 1'b1;
                        bst    <= B_WR_LOAD;
                    end
                end

            /* The controller latches the first word at the moment it accepts the request, so that
             * word has to exist before req_valid goes up. */
            /* Load one word: two cycles to fill the registered read, then eight shifts. Byte 0 ends
             * in the low byte of the word, which is the beat the controller sends first. */
            B_WR_LOAD: begin
                if (wcnt == 10) begin
                    buf_addr <= wnext;
                end else begin
                    buf_addr <= buf_addr + 1'b1;
                    if (wcnt <= 8) wacc <= {buf_rdata, wacc[63:8]};
                end
                wcnt <= wcnt - 1'b1;
                if (wcnt == 1) begin
                    /* Present the word NOW, before anyone asks for it: the controller latches
                     * wd_data on the same cycle it pulses wd_take. */
                    wd_data  <= {buf_rdata, wacc[63:8]};
                    wr_ready <= 1'b1;
                    wnext    <= wnext + 8;
                    bst      <= wfirst ? B_WR_REQ : B_WR_HOLD;
                end
            end

            B_WR_REQ: begin
                req_write <= 1'b1;
                req_bank  <= a_bank;
                req_row   <= a_row;
                req_col   <= {a_brst, 3'b000};
                req_len   <= (wleft > REQ_MAX[7:0]) ? REQ_MAX[7:0] : wleft;
                req_valid <= 1'b1;
                if (req_valid && req_ready) begin
                    /* The controller latches word 0 as it accepts, so that word is consumed here and
                     * the take it emits a cycle later is only an acknowledgement. */
                    req_valid <= 1'b0;
                    wr_ready  <= 1'b0;
                    wfirst    <= 1'b0;
                    wleft     <= wleft - 1'b1;
                    wcnt      <= 10;
                    bst       <= (wleft <= 1) ? B_IDLE : B_WR_LOAD;
                end
            end

            /* A word is assembled and waiting. The controller takes one per burst, every sixteen
             * fabric clocks, and a load needs ten, so the next one is always ready in time. */
            B_WR_HOLD:
                if (wd_take && wr_ready) begin
                    wr_ready <= 1'b0;
                    wleft    <= wleft - 1'b1;
                    if (wleft <= 1) begin
                        /* Release on the last word rather than waiting for the controller to finish
                         * the final burst. The next request cannot overtake it: req_ready will not
                         * assert again until the controller is genuinely idle. */
                        bst <= B_IDLE;
                    end else begin
                        wcnt <= 10;
                        bst  <= B_WR_LOAD;
                    end
                end

            default: bst <= B_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
