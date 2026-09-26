// review/experiments/par_rx_filt.v -- REVIEW PROPOSAL ONLY (not part of rtl/): rtl/par_rx.v with
// a STROBE glitch filter. Identical ports/behaviour except that a STROBE level change is only
// accepted once the synchronised level has been seen in 3 consecutive samples
// (stb_s2 == stb_s3 == stb_s4 != stb_acc). D/SOR are taken from d_s2/sor_s2 at the acceptance
// clock, i.e. D sampled 2 clocks after the first new-level sample = at most 3 clocks after the
// STROBE edge (<= 28 ns at 106.25 MHz, <= 20 ns at 148.75 MHz) -- well inside the 60 ns hold of
// SPEC section 3. STROBE pulses shorter than 2 core periods (ringback, crosstalk) can never
// produce a transfer.
`include "gpu_defs.vh"

module par_rx_filt #(
    parameter [23:0] ACTIVE_CYCLES = 24'd10625000,
    parameter [10:0] BUSY_FREE     = `T_BUSY_FREE
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
    (* ASYNC_REG = "TRUE" *) reg [15:0] d_s1   = 16'd0;
    (* ASYNC_REG = "TRUE" *) reg [15:0] d_s2   = 16'd0;
    (* ASYNC_REG = "TRUE" *) reg        sor_s1 = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg        sor_s2 = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg        stb_s1 = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg        stb_s2 = 1'b0;
    reg                                 stb_s3 = 1'b0;
    reg                                 stb_s4 = 1'b0;
    reg                                 stb_acc = 1'b0;   // last accepted STROBE level

    always @(posedge clk) begin
        d_s1   <= pin_d;
        d_s2   <= d_s1;
        sor_s1 <= pin_sor;
        sor_s2 <= sor_s1;
        stb_s1 <= pin_strobe;
        stb_s2 <= stb_s1;
        stb_s3 <= stb_s2;
        stb_s4 <= stb_s3;
        if (stb_s2 == stb_s3 && stb_s3 == stb_s4)
            stb_acc <= stb_s2;
    end

    wire xfer = (stb_s2 == stb_s3) && (stb_s3 == stb_s4) && (stb_s2 != stb_acc);

    reg        phase;
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
