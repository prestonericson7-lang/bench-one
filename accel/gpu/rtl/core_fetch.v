// core_fetch.v -- per-strip list walker.
//
// For strip `strip` of the list starting at ring slot `lstart` with `nrec` records: scans the
// side RAM (one slot per cycle), queues the slots of records whose strip range contains the
// strip (in list order), and fetches each queued record (one 720-bit read of the list RAM) into
// the stage buffer, unpacked back to words 0..22 (core_listram describes the packing; word 23
// is always 0 and unused). The consumer takes the stage (stage_release) and the next queued
// record is fetched. `done` = everything for this strip has been scanned, fetched and released.
// Side RAM read latency 2 cycles (core_ram OREG=1), list RAM read latency 3 (core_listram).
module core_fetch #(
    parameter SAW = 11                     // log2(ring slots)
) (
    input  wire              clk,
    input  wire              rst,
    input  wire              start,       // pulse: begin strip (previous strip must be done)
    input  wire [5:0]        strip,
    input  wire [SAW-1:0]    lstart,
    input  wire [10:0]       nrec,
    output wire              done,
    // side RAM read port
    output reg               s_re,
    output reg  [SAW-1:0]    s_raddr,
    input  wire [11:0]       s_rdata,
    // list RAM read port
    output reg               l_re,
    output reg  [SAW-1:0]    l_raddr,
    input  wire [719:0]      l_rdata,
    // stage buffer
    output reg               stage_valid,
    output wire [23*32-1:0]  stage_words,
    input  wire              stage_release
);
    // ------------------------------------------------------------------ scanner
    reg           scanning;
    reg [10:0]    si;            // next list index to issue
    reg [SAW-1:0] ss;            // ring slot of list index si
    reg [SAW-1:0] sa1, sa2;      // slot pipeline aligned with sv1 / sv2
    reg           sv1, sv2;

    // match queue (slots), depth 4
    reg [SAW-1:0] mq [0:3];
    reg [1:0]     mq_wp, mq_rp;
    reg [2:0]     mq_cnt;

    wire [5:0] smin = s_rdata[11:6];
    wire [5:0] smax = s_rdata[5:0];
    wire       hit  = sv2 && (smin <= strip) && (strip <= smax);

    wire [2:0] inflight  = {2'b00, s_re} + {2'b00, sv1} + {2'b00, sv2};
    wire       can_issue = scanning && (si != nrec) && ((mq_cnt + inflight) < 3'd4);

    wire       scan_done = !scanning || ((si == nrec) && !s_re && !sv1 && !sv2);

    // ------------------------------------------------------------------ fetcher
    reg           fetching;      // one record read in flight
    reg           lv1, lv2;      // l_re pipeline (data valid with lv3)
    // lv3 enables the 720 stage flip-flops next to the 40 list block RAMs: let synthesis
    // replicate it (a single driver was the worst routed path, 5.7 ns of routing)
    (* max_fanout = 32 *)
    reg           lv3;
    reg [719:0]   stg;

    wire mq_pop  = !fetching && !stage_valid && (mq_cnt != 3'd0);
    wire mq_push = hit;

    assign done = scan_done && (mq_cnt == 3'd0) && !fetching && !stage_valid;

    // ------------------------------------------------------------------ unpack (wires only)
    wire [79:0] xb;
    genvar gc;
    generate
        for (gc = 0; gc < 10; gc = gc + 1) begin : g_unpack
            assign stage_words[(3 + 2*gc)*32 +: 32] = stg[gc*72 +: 32];
            assign stage_words[(4 + 2*gc)*32 +: 32] = stg[gc*72 + 32 +: 32];
            assign xb[gc*8 +: 8]                    = stg[gc*72 + 64 +: 8];
        end
    endgenerate
    // xb = { w2[31:0], w1[21:0], hdr[25:0] }, hdr = { is_sprite, f27, f25, f24, w0[21:0] }
    assign stage_words[0*32 +: 32] = {2'b00, xb[25], !xb[25], xb[24], 1'b0, xb[23], xb[22],
                                      2'b00, xb[21:0]};
    assign stage_words[1*32 +: 32] = {10'd0, xb[47:26]};
    assign stage_words[2*32 +: 32] = xb[79:48];

    always @(posedge clk) begin
        if (mq_push)
            mq[mq_wp] <= sa2;
    end

    always @(posedge clk) begin
        if (lv3)
            stg <= l_rdata;
    end

    always @(posedge clk) begin
        if (rst) begin
            scanning <= 1'b0;
            si       <= 11'd0;
            ss       <= {SAW{1'b0}};
            sa1      <= {SAW{1'b0}};
            sa2      <= {SAW{1'b0}};
            s_re     <= 1'b0;
            s_raddr  <= {SAW{1'b0}};
            sv1      <= 1'b0;
            sv2      <= 1'b0;
            mq_wp    <= 2'd0;
            mq_rp    <= 2'd0;
            mq_cnt   <= 3'd0;
            fetching <= 1'b0;
            l_re     <= 1'b0;
            l_raddr  <= {SAW{1'b0}};
            lv1      <= 1'b0;
            lv2      <= 1'b0;
            lv3      <= 1'b0;
            stage_valid <= 1'b0;
        end else begin
            // ---------------- scanner
            sv1 <= s_re;
            sv2 <= sv1;
            sa1 <= s_raddr;
            sa2 <= sa1;
            if (start) begin
                scanning <= 1'b1;
                si       <= 11'd0;
                ss       <= lstart;
                s_re     <= 1'b0;
            end else if (can_issue) begin
                s_re    <= 1'b1;
                s_raddr <= ss;
                si      <= si + 11'd1;
                ss      <= ss + 1'b1;
            end else begin
                s_re <= 1'b0;
                if (scanning && (si == nrec) && !s_re && !sv1 && !sv2)
                    scanning <= 1'b0;
            end

            // ---------------- match queue
            if (mq_push)
                mq_wp <= mq_wp + 2'd1;
            if (mq_pop)
                mq_rp <= mq_rp + 2'd1;
            mq_cnt <= mq_cnt + (mq_push ? 3'd1 : 3'd0) - (mq_pop ? 3'd1 : 3'd0);

            // ---------------- fetcher: one read per record
            lv1  <= l_re;
            lv2  <= lv1;
            lv3  <= lv2;
            l_re <= mq_pop;
            if (mq_pop) begin
                fetching <= 1'b1;
                l_raddr  <= mq[mq_rp];
            end
            if (lv3) begin
                fetching    <= 1'b0;
                stage_valid <= 1'b1;
            end
            if (stage_release)
                stage_valid <= 1'b0;
        end
    end
endmodule
