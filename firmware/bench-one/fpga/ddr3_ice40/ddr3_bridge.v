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
    parameter integer CK_DIV   = 4
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
    output reg                  err_overrun   /* a burst arrived before the last one was stored */
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

    localparam [3:0] B_IDLE  = 0,
                     B_RD_REQ = 1, B_RD_WAIT = 2,
                     B_WR_WAIT= 3, B_WR_PRIME= 4, B_WR_REQ = 5, B_WR_FEED = 6;

    reg [3:0]          bst;
    reg [31:0]         addr;        /* effective address of the next burst to transfer */
    reg [BUF_BITS-1:0] fill;        /* next buffer byte to write (reads) */
    reg [8:0]          need;        /* bursts still to request for this transaction */
    reg [7:0]          got;         /* bursts still expected from the current request */
    reg [63:0]         racc;        /* word being unpacked into the buffer */
    reg [3:0]          rcnt;        /* bytes left to store from racc */
    reg [63:0]         wacc;        /* word being assembled from the buffer */
    reg [3:0]          wcnt;        /* bytes still to fetch into wacc */
    reg [7:0]          wleft;       /* bursts still to hand the controller */
    reg [BUF_BITS-1:0] wpos;        /* next buffer byte to read (writes) */

    assign idle = (bst == B_IDLE);

    /* Split the flat address. Registered values, so this is a decode of `addr` and not of a live
     * input; that matters because addr advances as the fetch walks across rows. */
    wire [BA_BITS-1:0]  a_bank = addr[12:10];
    wire [ROW_BITS-1:0] a_row  = addr[27:13];
    wire [6:0]          a_brst = addr[9:3];
    wire [8:0]          to_row_end = 9'd128 - {2'b00, a_brst};
    wire [8:0]          this_len   = (need < to_row_end) ? need : to_row_end;

    always @(posedge sys_clk) begin
        buf_we <= 1'b0;

        if (sys_rst) begin
            bst <= B_IDLE; addr <= 32'd0; fill <= 0; need <= 0; got <= 0;
            racc <= 64'd0; rcnt <= 0; wacc <= 64'd0; wcnt <= 0; wleft <= 0; wpos <= 0;
            buf_addr <= 0; buf_wdata <= 8'h00;
            req_valid <= 1'b0; req_write <= 1'b0;
            req_bank <= 0; req_row <= 0; req_col <= 0; req_len <= 0;
            wd_data <= 64'd0; err_overrun <= 1'b0;
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
                buf_addr  <= fill;
                buf_wdata <= racc[7:0];
                buf_we    <= 1'b1;
                racc      <= {8'h00, racc[63:8]};
                fill      <= fill + 1'b1;
                rcnt      <= rcnt - 1'b1;
            end

            case (bst)
            B_IDLE:
                if (start_edge && init_done) begin
                    addr  <= q_addr + q_win_base;
                    fill  <= 0;
                    rcnt  <= 0;
                    if (q_is_write) begin
                        bst <= B_WR_WAIT;
                    end else begin
                        /* Fill the entire buffer, whatever the transaction turns out to be. The
                         * Teensy's AHB buffer can ask for 512 bytes and we have no way to know in
                         * advance, so the only safe answer is to have all 1 KB ready. */
                        need <= (1 << BUF_BITS) >> 3;
                        bst  <= B_RD_REQ;
                    end
                end

            /* ---- read: issue requests until the buffer is full, crossing rows as needed ---- */
            B_RD_REQ:
                if (need == 0) begin
                    bst <= B_IDLE;
                end else begin
                    req_write <= 1'b0;
                    req_bank  <= a_bank;
                    req_row   <= a_row;
                    req_col   <= {a_brst, 3'b000};
                    req_len   <= (this_len > REQ_MAX) ? REQ_MAX[7:0] : this_len[7:0];
                    req_valid <= 1'b1;            /* HELD until taken; see the header */
                    if (req_valid && req_ready) begin
                        req_valid <= 1'b0;
                        got       <= (this_len > REQ_MAX) ? REQ_MAX[7:0] : this_len[7:0];
                        need      <= need - ((this_len > REQ_MAX) ? REQ_MAX : this_len);
                        addr      <= addr + (((this_len > REQ_MAX) ? REQ_MAX : this_len) << 3);
                        bst       <= B_RD_WAIT;
                    end
                end

            B_RD_WAIT: begin
                if (rd_valid && got != 0) got <= got - 1'b1;
                /* Wait for the last word to be unpacked as well as received, or the next request
                 * would overwrite racc mid-store. */
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
                    wpos  <= 0;
                    if (q_wr_count[BUF_BITS-1:3] == 0) bst <= B_IDLE;
                    else begin
                        buf_addr <= 0;
                        /* NINE, not eight. The buffer's read is registered, so the first cycle only
                         * presents an address and its byte does not appear until the next one. A
                         * count of eight therefore performs seven shifts, leaving the low byte of
                         * every word at zero and pushing all eight real bytes up by one position --
                         * which on hardware writes a zero into every address divisible by eight and
                         * loses the last byte of each burst. */
                        wcnt     <= 9;
                        bst      <= B_WR_PRIME;
                    end
                end

            /* The controller latches the first word at the moment it accepts the request, so that
             * word has to exist before req_valid goes up. */
            B_WR_PRIME: begin
                /* buf_rdata lags buf_addr by one clock, so the address runs one ahead of the capture
                 * and the very first cycle shifts nothing. Counting 9 down to 1 performs exactly
                 * eight shifts, which leaves byte 0 in the low byte of the word -- the position the
                 * controller sends first. */
                buf_addr <= wpos + 1'b1;
                wpos     <= wpos + 1'b1;
                if (wcnt != 9) wacc <= {buf_rdata, wacc[63:8]};
                wcnt <= wcnt - 1'b1;
                if (wcnt == 1) bst <= B_WR_REQ;
            end

            B_WR_REQ: begin
                wd_data   <= wacc;
                req_write <= 1'b1;
                req_bank  <= a_bank;
                req_row   <= a_row;
                req_col   <= {a_brst, 3'b000};
                req_len   <= (wleft > REQ_MAX[7:0]) ? REQ_MAX[7:0] : wleft;
                req_valid <= 1'b1;
                if (req_valid && req_ready) begin
                    req_valid <= 1'b0;
                    wcnt      <= 9;               /* nine for the same reason as the prime loop */
                    bst       <= B_WR_FEED;
                end
            end

            B_WR_FEED: begin
                /* Assemble the next word while the current one is on the wire. A burst lasts sixteen
                 * fabric clocks and this needs eight, so it is always ready in time. */
                if (wcnt != 0) begin
                    buf_addr <= wpos + 1'b1;
                    wpos     <= wpos + 1'b1;
                    if (wcnt != 9) wacc <= {buf_rdata, wacc[63:8]};
                    wcnt     <= wcnt - 1'b1;
                end
                if (wd_take) begin
                    wd_data <= wacc;
                    wcnt    <= 9;
                    if (wleft != 0) wleft <= wleft - 1'b1;
                    /* Release on the last take rather than waiting for the controller to finish the
                     * final burst. The next request cannot overtake it: req_ready will not assert
                     * again until the controller is genuinely idle. */
                    if (wleft <= 1) bst <= B_IDLE;
                end
            end

            default: bst <= B_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
