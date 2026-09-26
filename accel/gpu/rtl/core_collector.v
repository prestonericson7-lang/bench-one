// core_collector.v -- command collector (SPEC section 7).
//
// Reads 33-bit {sor, word} entries from the two FWFT command FIFOs, assembles 24-word records,
// and appends TRI/SPRITE records to the list being filled. For each appended record it also
// writes a 12-bit "strip range" entry {first_strip[5:0], last_strip[5:0]} into the side RAM so
// the rasteriser can skip records that do not touch a strip without reading them (an empty
// record gets {63,0}).
//
//  * A list part is taken from each enabled source, Teensy first, then PS, until that source's
//    END record. The list completes when every enabled source has delivered its END.
//  * A disabled source is drained continuously (dropped_cnt += words).
//  * sor=1 mid-record: partial record discarded (bad += 1), new record starts.
//    sor=0 while no record is open: discarded, bad += 1 once per run.
//    Unknown record types: bad += 1. NOP ignored. List full (LIST_SLOTS records): record
//    dropped, overflow += 1.
//  * If the active source changes while a record is open (enable bits changed mid-record) the
//    partial record is discarded (bad += 1).  If the list completes while a record from a
//    source that was disabled meanwhile is still open, that partial record is discarded too.
//  * soft_reset: open record, list being filled and a complete-but-not-yet-rendered list are
//    discarded. The list the rasteriser is currently rendering is kept and finishes normally.
//    Status counters are NOT cleared by soft_reset (only by rst).
//  * frame_no (SPEC 13.1): per list, w1 of the END that completed it -- the PS END if SRC_PS is
//    enabled at completion, else the Teensy END.
//
// Record storage: a ring of RING record slots shared by the two lists (core_listram, 720 bits
// per slot, packing described there). Lists are contiguous in the ring: the list being filled
// starts where the previous list ended (head - cnt) and slots are freed a whole list at a time
// when the rasteriser frees its list. Each list still holds up to LIST_SLOTS records (SPEC 7);
// only the sum of both lists is limited to RING. When the ring is full the collector stops
// taking words (back-pressure through the FIFOs: Teensy BUSY / PS_FIFO_FREE) until the
// rendered list is freed; the result is identical, only later. The stall is applied only
// between records (no record open) and only while the list being filled can still grow; words
// that start an END/NOP/garbage record also wait then (they cannot be told apart before being
// read), which is harmless: the other list is complete or rendering and is freed without the
// collector. The full flag is registered (t_rd/p_rd timing) and conservative by one slot:
// no_space_r = (occupancy >= RING - 1) one cycle earlier, which covers the current occupancy
// because it grows by at most one per cycle. RING - 1 >= LIST_SLOTS guarantees progress (a
// single list always fits).
//
// Ping-pong: c_sel = list being filled, r_sel = list the rasteriser renders next / now,
// full[i] = list i is complete (waiting or being rendered).
module core_collector #(
    parameter LIST_SLOTS = 1536,   // records per list, <= 2047 and <= RING
    parameter RING       = 2048,   // record slots shared by both lists (power of two)
    parameter SAW        = 11      // log2(RING)
) (
    input  wire            clk,
    input  wire            rst,
    input  wire            soft_reset,
    input  wire            src_teensy_en,
    input  wire            src_ps_en,
    // FIFOs (FWFT)
    input  wire            t_empty,
    input  wire [32:0]     t_dout,
    output wire            t_rd,
    input  wire            p_empty,
    input  wire [32:0]     p_dout,
    output wire            p_rd,
    // list RAM column write port
    output reg             lw_en,
    output reg [3:0]       lw_col,
    output reg [SAW-1:0]   lw_addr,
    output reg [71:0]      lw_data,
    // side RAM write port
    output reg             sw_en,
    output reg [SAW-1:0]   sw_addr,
    output reg [11:0]      sw_data,
    // rasteriser interface (values of list r_sel)
    output wire            r_avail,      // list r_sel is complete and not yet taken
    output wire [SAW-1:0]  r_start,      // first slot of list r_sel
    output wire [10:0]     r_cnt,        // records in list r_sel
    output wire [31:0]     r_fno,        // frame_no of list r_sel
    input  wire            r_take,       // pulse, only when r_avail
    input  wire            r_free,       // pulse, rasteriser done with list r_sel
    // status
    output wire            wait_teensy,
    output wire            wait_ps,
    output reg [31:0]      list_overflow_cnt,
    output reg [31:0]      bad_record_cnt,
    output reg [31:0]      dropped_cnt
);
    localparam [10:0]  SLOTS = LIST_SLOTS;
    localparam [12:0]  RINGM1 = RING - 1;

    generate
        if (LIST_SLOTS > RING - 1 || (1 << SAW) != RING || LIST_SLOTS > 2047) begin : g_bad_param
            /* verilator lint_off DECLFILENAME */
            core_collector_bad_parameters u_error ();   // elaboration error on purpose
            /* verilator lint_on DECLFILENAME */
        end
    endgenerate

    // ---------------------------------------------------------------- list ownership
    reg           c_sel;
    reg [1:0]     full;
    reg           r_sel;
    reg           r_act;
    reg [10:0]    cnt0, cnt1;
    reg [SAW-1:0] start0, start1;
    reg [31:0]    fno0, fno1;

    // ---------------------------------------------------------------- fill state
    reg [10:0]    cnt;          // records in list being filled
    reg [SAW-1:0] head;         // next free slot (= start of the list being filled + cnt)
    reg           rec_open;
    reg [4:0]     wcnt;         // words received of the open record (1..23)
    reg [3:0]     rtype;
    reg           rstore;       // open record is TRI/SPRITE and the list has room: store it
    reg           rsrc;         // 0 = Teensy, 1 = PS
    reg           bad_run;
    reg           bad_run_src;
    reg           got_t, got_p;
    /* verilator lint_off UNUSED */
    reg [21:0]    f0, f1, f2;   // captured w0/w1/w2 [21:0]
    /* verilator lint_on UNUSED */
    reg [31:0]    w1q;          // full w1 of the open record (END: frame number)
    reg [31:0]    t_fno, p_fno; // frame numbers of the ENDs received for the list being filled
    reg [11:0]    side_q;
    reg [25:0]    hdr;          // packed w0 (see core_listram)
    reg [79:0]    xsh;          // extra bits X, shifted right 8 per column written
    reg [31:0]    prev;         // previous (odd-index) word
    reg [3:0]     col;          // next column to write

    wire list_open = !full[c_sel];
    wire t_cur     = list_open && src_teensy_en && !got_t;
    wire p_cur     = list_open && src_ps_en && !got_p && !t_cur;
    wire act_valid = t_cur || p_cur;
    wire act_src   = p_cur;                       // 0 = Teensy, 1 = PS
    wire mismatch  = rec_open && act_valid && (act_src != rsrc);

    wire slot_free = (cnt < SLOTS);

    // ring occupancy: the other list (if complete / being rendered) + the list being filled
    wire [10:0] cnt_o    = c_sel ? cnt0 : cnt1;
    wire [12:0] used     = (full[~c_sel] ? {2'b00, cnt_o} : 13'd0) + {2'b00, cnt};
    reg         no_space_r;                  // registered: (used >= RING - 1) last cycle
    reg         slot_free_r;                 // registered copy of slot_free, for the stall only
    wire        stall    = !rec_open && slot_free_r && no_space_r;
    // slot_free_r is one cycle late: late 1 (list just filled up) only adds a stall cycle; late 0
    // happens only when cnt was just cleared by a list completion or a soft reset, and then the
    // ring cannot be full (see no_space_r) or the collector has no open list to fill.

    always @(posedge clk) begin
        if (rst) begin
            no_space_r  <= 1'b0;
            slot_free_r <= 1'b1;
        end else begin
            no_space_r  <= (used >= RINGM1);
            slot_free_r <= slot_free;
        end
    end

    wire t_drain = !src_teensy_en && !t_empty;
    wire p_drain = !src_ps_en && !p_empty;
    wire t_take  = t_cur && !t_empty && !mismatch && !stall;
    wire p_take  = p_cur && !p_empty && !mismatch && !stall;

    assign t_rd = !soft_reset && (t_drain || t_take);
    assign p_rd = !soft_reset && (p_drain || p_take);

    wire        w_valid = t_take || p_take;
    wire [32:0] w_in    = t_take ? t_dout : p_dout;
    wire        w_sor   = w_in[32];
    wire [3:0]  w_type  = w_in[31:28];

    wire complete = list_open && (src_teensy_en || src_ps_en) &&
                    (!src_teensy_en || got_t) && (!src_ps_en || got_p);

    wire bad_run_eff = bad_run && (bad_run_src == act_src);

    assign r_avail     = full[r_sel] && !r_act && !soft_reset;
    assign r_start     = r_sel ? start1 : start0;
    assign r_cnt       = r_sel ? cnt1 : cnt0;
    assign r_fno       = r_sel ? fno1 : fno0;
    assign wait_teensy = t_cur;
    assign wait_ps     = p_cur;

    // ---------------------------------------------------------------- strip range of the record
    // TRI:    rows ymin..min(ymax,719), cols xmin..min(xmax,1279)
    // SPRITE: rows y..min(y+h-1,719) (h>=1, y<720), cols need x<1280 and (w & ~3) != 0
    wire [10:0] t_xmin = f0[10:0];
    wire [10:0] t_xmax = f0[21:11];
    wire [10:0] t_ymin = f1[10:0];
    wire [10:0] t_ymax = f1[21:11];
    wire [10:0] t_ye   = (t_ymax > 11'd719)  ? 11'd719  : t_ymax;
    wire [10:0] t_xe   = (t_xmax > 11'd1279) ? 11'd1279 : t_xmax;
    wire        t_empty_rec = (t_ymin > t_ye) || (t_xmin > t_xe);

    wire [10:0] s_x    = {f1[10:2], 2'b00};
    wire [10:0] s_y    = f1[21:11];
    wire [10:0] s_w    = {f2[10:2], 2'b00};
    wire [10:0] s_h    = f2[21:11];
    wire [11:0] s_yend = {1'b0, s_y} + {1'b0, s_h} - 12'd1;
    /* verilator lint_off UNUSED */
    wire [11:0] s_ye   = (s_yend > 12'd719) ? 12'd719 : s_yend;
    /* verilator lint_on UNUSED */
    wire        s_empty_rec = (s_y > 11'd719) || (s_h == 11'd0) || (s_x > 11'd1279) || (s_w == 11'd0);

    always @(posedge clk) begin
        if (rtype == 4'd2) // SPRITE
            side_q <= s_empty_rec ? 12'hFC0 : {s_y[9:4], s_ye[9:4]};
        else
            side_q <= t_empty_rec ? 12'hFC0 : {t_ymin[9:4], t_ye[9:4]};
    end

    // ---------------------------------------------------------------- main
    always @(posedge clk) begin
        if (rst) begin
            c_sel    <= 1'b0;
            full     <= 2'b00;
            r_sel    <= 1'b0;
            r_act    <= 1'b0;
            cnt0     <= 11'd0;
            cnt1     <= 11'd0;
            start0   <= {SAW{1'b0}};
            start1   <= {SAW{1'b0}};
            fno0     <= 32'd0;
            fno1     <= 32'd0;
            w1q      <= 32'd0;
            t_fno    <= 32'd0;
            p_fno    <= 32'd0;
            cnt      <= 11'd0;
            head     <= {SAW{1'b0}};
            rec_open <= 1'b0;
            wcnt     <= 5'd0;
            rtype    <= 4'd0;
            rstore   <= 1'b0;
            rsrc     <= 1'b0;
            bad_run  <= 1'b0;
            bad_run_src <= 1'b0;
            got_t    <= 1'b0;
            got_p    <= 1'b0;
            f0 <= 22'd0; f1 <= 22'd0; f2 <= 22'd0;
            hdr      <= 26'd0;
            xsh      <= 80'd0;
            prev     <= 32'd0;
            col      <= 4'd0;
            lw_en    <= 1'b0;
            lw_col   <= 4'd0;
            lw_addr  <= {SAW{1'b0}};
            lw_data  <= 72'd0;
            sw_en    <= 1'b0;
            sw_addr  <= {SAW{1'b0}};
            sw_data  <= 12'd0;
            list_overflow_cnt <= 32'd0;
            bad_record_cnt    <= 32'd0;
            dropped_cnt       <= 32'd0;
        end else begin
            lw_en <= 1'b0;
            sw_en <= 1'b0;

            // words drained from disabled sources (t_rd/p_rd are gated by soft_reset, so count
            // only real pops)
            if (!soft_reset)
                dropped_cnt <= dropped_cnt + (t_drain ? 32'd1 : 32'd0) + (p_drain ? 32'd1 : 32'd0);

            // ---------------- rasteriser take / free
            if (r_take && r_avail)
                r_act <= 1'b1;
            if (r_free) begin
                r_act <= 1'b0;
                r_sel <= ~r_sel;
            end

            if (soft_reset) begin
                // keep only the list being rendered (unless it is being freed right now)
                full[0]  <= full[0] && r_act && !r_free && (r_sel == 1'b0);
                full[1]  <= full[1] && r_act && !r_free && (r_sel == 1'b1);
                c_sel    <= r_act ? ~r_sel : r_sel;
                cnt      <= 11'd0;
                // the next list starts right after the kept list (ring empty otherwise)
                if (r_act && !r_free)
                    head <= r_sel ? (start1 + cnt1[SAW-1:0]) : (start0 + cnt0[SAW-1:0]);
                rec_open <= 1'b0;
                wcnt     <= 5'd0;
                bad_run  <= 1'b0;
                got_t    <= 1'b0;
                got_p    <= 1'b0;
            end else begin
                if (r_free)
                    full[r_sel] <= 1'b0;

                if (complete) begin
                    full[c_sel] <= 1'b1;
                    if (c_sel) begin
                        cnt1   <= cnt;
                        start1 <= head - cnt[SAW-1:0];
                        fno1   <= src_ps_en ? p_fno : t_fno;
                    end else begin
                        cnt0   <= cnt;
                        start0 <= head - cnt[SAW-1:0];
                        fno0   <= src_ps_en ? p_fno : t_fno;
                    end
                    c_sel    <= ~c_sel;
                    cnt      <= 11'd0;
                    got_t    <= 1'b0;
                    got_p    <= 1'b0;
                    bad_run  <= 1'b0;
                    if (rec_open) begin
                        rec_open <= 1'b0;
                        bad_record_cnt <= bad_record_cnt + 32'd1;
                    end
                end else if (mismatch) begin
                    // active source changed under an open record: discard it
                    rec_open <= 1'b0;
                    bad_record_cnt <= bad_record_cnt + 32'd1;
                end else if (w_valid) begin
                    if (w_sor) begin
                        if (rec_open)
                            bad_record_cnt <= bad_record_cnt + 32'd1;
                        rec_open <= 1'b1;
                        wcnt     <= 5'd1;
                        rtype    <= w_type;
                        rstore   <= slot_free && ((w_type == 4'd1) || (w_type == 4'd2));
                        rsrc     <= act_src;
                        bad_run  <= 1'b0;
                        f0       <= w_in[21:0];
                        hdr      <= {w_in[29], w_in[27], w_in[25], w_in[24], w_in[21:0]};
                        col      <= 4'd0;
                    end else if (rec_open) begin
                        if (wcnt == 5'd1) begin
                            f1  <= w_in[21:0];
                            w1q <= w_in[31:0];
                        end
                        if (wcnt == 5'd2) begin
                            f2  <= w_in[21:0];
                            xsh <= {w_in[31:0], f1, hdr};
                        end
                        if (wcnt[0])
                            prev <= w_in[31:0];
                        if (!wcnt[0] && (wcnt >= 5'd4) && (wcnt <= 5'd22)) begin
                            // column (wcnt-4)/2 = { X byte, w(wcnt), w(wcnt-1) }
                            lw_en   <= rstore;
                            lw_col  <= col;
                            lw_addr <= head;
                            lw_data <= {xsh[7:0], w_in[31:0], prev};
                            xsh     <= {8'd0, xsh[79:8]};
                            col     <= col + 4'd1;
                        end
                        if (wcnt == 5'd23) begin
                            rec_open <= 1'b0;
                            case (rtype)
                                4'd1, 4'd2: begin      // TRI, SPRITE
                                    if (slot_free) begin
                                        cnt     <= cnt + 11'd1;
                                        head    <= head + 1'b1;
                                        sw_en   <= 1'b1;
                                        sw_addr <= head;
                                        sw_data <= side_q;
                                    end else begin
                                        list_overflow_cnt <= list_overflow_cnt + 32'd1;
                                    end
                                end
                                4'd0: ;                // NOP
                                4'd15: begin           // END
                                    if (rsrc) begin
                                        got_p <= 1'b1;
                                        p_fno <= w1q;
                                    end else begin
                                        got_t <= 1'b1;
                                        t_fno <= w1q;
                                    end
                                end
                                default:
                                    bad_record_cnt <= bad_record_cnt + 32'd1;
                            endcase
                        end else begin
                            wcnt <= wcnt + 5'd1;
                        end
                    end else begin
                        if (!bad_run_eff)
                            bad_record_cnt <= bad_record_cnt + 32'd1;
                        bad_run     <= 1'b1;
                        bad_run_src <= act_src;
                    end
                end
            end
        end
    end
endmodule
