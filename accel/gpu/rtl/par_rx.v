// ---------------------------------------------------------------------------------------------
// par_rx -- Teensy 16-bit parallel bus receiver (SPEC section 3, rtl/INTERFACES.md)
//
// Bus protocol (Teensy is the only driver of D/SOR/STROBE): drive D[15:0] and SOR, wait >= 30 ns,
// toggle STROBE (every edge, rising or falling, is one transfer), hold D/SOR >= 60 ns.
// Two transfers per 32-bit word, low half first. SOR = 1 on the first transfer of a record.
//
// Receiver (core clock, 148.75 MHz = 6.723 ns period T):
//   * 2-FF synchronisers (ASYNC_REG) on all 18 inputs; STROBE continues through stb_s3/stb_s4.
//     Synchronisers are never reset (resetting them could create a false STROBE edge).
//   * STROBE glitch filter (review R1-01): a new STROBE level is ACCEPTED only when the
//     synchronised level is the same in 3 consecutive samples (stb_s2 == stb_s3 == stb_s4) and
//     differs from the last accepted level stb_acc. One acceptance = one transfer. A pulse
//     shorter than 2 T (13.4 ns) can be in at most 2 consecutive samples, so ringback/crosstalk
//     glitches never produce (or cancel) a transfer; a level held > 3 T (20.2 ns, plus the
//     metastability window of the first sample) is always accepted. Without the filter one
//     glitch added a duplicated half word and silently shifted the rest of the record by one
//     word.
//   * In the accepting cycle D/SOR are taken from d_s2/sor_s2 = the pins as sampled 2 clocks
//     after the first sample that saw the new STROBE level: 2..3 T after the STROBE edge
//     (+1 T if that first sample went metastable), i.e. <= 27 ns at 148.75 MHz. D/SOR were set
//     >= 30 ns BEFORE the edge and are held >= 60 ns after it, so they are stable there.
//   * SOR = 1 on a transfer starts a new word (low half) whatever the current phase, so a record
//     start always re-aligns the half-word phase. A dangling low half is discarded.
//   * Assembled word pushed as f_din = {sor_of_low_half, hi, lo} with a 1-cycle f_wr pulse.
//   * pin_busy (registered) = rst || (f_free < 64). Initial value 1 (busy until configured+reset).
//   * active = a transfer was accepted within the last ACTIVE_CYCLES core clocks (~100 ms).
//   * words_rx counts assembled words (= pushes).
// Latency: with k1 = the first clock edge after the STROBE edge, the transfer is accepted at
// k1 + 4 T and f_wr is high in the following cycle: 4..5 T after the pin edge (+1 T if the
// first STROBE sample went metastable). The old unfiltered receiver took 2..3 T.
// ---------------------------------------------------------------------------------------------
`include "gpu_defs.vh"

module par_rx #(
    parameter [23:0] ACTIVE_CYCLES = 24'd14875000,          // 100 ms at 148.75 MHz
    parameter [10:0] BUSY_FREE     = `T_BUSY_FREE           // BUSY while free < this
) (
    input  wire        clk,
    input  wire        rst,
    input  wire [15:0] pin_d,
    input  wire        pin_sor,
    input  wire        pin_strobe,
    output reg         pin_busy,
    output reg         f_wr,
    output reg  [32:0] f_din,
    input  wire [10:0] f_free,
    output reg  [31:0] words_rx,
    output reg         active
);
    // ---- synchronisers + STROBE filter (no reset) ---------------------------------------------
    (* ASYNC_REG = "TRUE" *) reg [15:0] d_s1   = 16'd0;
    (* ASYNC_REG = "TRUE" *) reg [15:0] d_s2   = 16'd0;
    (* ASYNC_REG = "TRUE" *) reg        sor_s1 = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg        sor_s2 = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg        stb_s1 = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg        stb_s2 = 1'b0;
    reg                                 stb_s3 = 1'b0;
    reg                                 stb_s4 = 1'b0;
    reg                                 stb_acc = 1'b0;   // last accepted STROBE level

    wire stb_stable = (stb_s2 == stb_s3) && (stb_s3 == stb_s4);

    always @(posedge clk) begin
        d_s1   <= pin_d;
        d_s2   <= d_s1;
        sor_s1 <= pin_sor;
        sor_s2 <= sor_s1;
        stb_s1 <= pin_strobe;
        stb_s2 <= stb_s1;
        stb_s3 <= stb_s2;
        stb_s4 <= stb_s3;
        if (stb_stable)
            stb_acc <= stb_s2;
    end

    wire xfer = stb_stable && (stb_s2 != stb_acc);   // one transfer per accepted STROBE level

    // ---- half-word assembly -------------------------------------------------------------------
    reg        phase;                     // 1 = low half held, waiting for the high half
    reg [15:0] lo;
    reg        wsor;
    reg [23:0] act_cnt;

    initial pin_busy = 1'b1;

    always @(posedge clk) begin
        pin_busy <= rst || (f_free < BUSY_FREE);
        f_wr     <= 1'b0;
        if (rst) begin
            phase    <= 1'b0;
            lo       <= 16'd0;
            wsor     <= 1'b0;
            f_din    <= 33'd0;
            words_rx <= 32'd0;
            act_cnt  <= 24'd0;
            active   <= 1'b0;
        end else begin
            if (xfer) begin
                if (sor_s2 || !phase) begin
                    lo    <= d_s2;
                    wsor  <= sor_s2;
                    phase <= 1'b1;
                end else begin
                    f_wr     <= 1'b1;
                    f_din    <= {wsor, d_s2, lo};
                    phase    <= 1'b0;
                    words_rx <= words_rx + 32'd1;
                end
            end
            if (xfer)
                act_cnt <= ACTIVE_CYCLES;
            else if (act_cnt != 24'd0)
                act_cnt <= act_cnt - 24'd1;
            active <= (act_cnt != 24'd0);
        end
    end

endmodule
