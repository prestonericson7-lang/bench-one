// tmds_encoder.v -- DVI 1.0 TMDS encoder (transition minimised, DC balanced), one channel.
//
// Exactly the algorithm of DVI 1.0 section 3.2.3 (figure 3-5):
//   stage 1: q_m = XOR/XNOR chain of d (XNOR if N1(d) > 4 or (N1(d) == 4 and d[0] == 0)),
//            q_m[8] = 1 for XOR, 0 for XNOR; N1(q_m[7:0]) registered.
//   stage 2: running-disparity selection of q_out, or the control token when de = 0
//            (cnt is cleared during blanking).
// Latency: 2 clocks from (d, c0, c1, de) to q.  q[0] is the first bit on the wire.
// Control tokens (q[9:0]): {c1,c0}=00 1101010100, 01 0010101011, 10 0101010100, 11 1010101011.
// The running disparity cnt is provably confined to {-8,-6,...,+8} (sim/tmds_ref.py bound);
// a 5-bit two's complement register holds it exactly (intermediate sums wrap harmlessly
// because every final value lies inside [-16,15]).
module tmds_encoder (
    input  wire       clk,
    input  wire       rst,        // synchronous, active high
    input  wire [7:0] d,
    input  wire       c0,
    input  wire       c1,
    input  wire       de,
    output reg  [9:0] q
);
    function [3:0] popcnt8;
        input [7:0] x;
        integer k;
        begin
            popcnt8 = 4'd0;
            for (k = 0; k < 8; k = k + 1)
                popcnt8 = popcnt8 + {3'd0, x[k]};
        end
    endfunction

    // ---------------- stage 1 ----------------
    wire [3:0] n1d = popcnt8(d);
    wire       use_xnor = (n1d > 4'd4) || ((n1d == 4'd4) && !d[0]);

    // q_m[0] = d[0]; q_m[i] = q_m[i-1] XOR d[i] (or XNOR)
    function [7:0] qm_calc;
        input [7:0] x;
        input       xn;
        integer k;
        begin
            qm_calc[0] = x[0];
            for (k = 1; k < 8; k = k + 1)
                qm_calc[k] = xn ? ~(qm_calc[k-1] ^ x[k]) : (qm_calc[k-1] ^ x[k]);
        end
    endfunction
    wire [7:0] qm = qm_calc(d, use_xnor);

    reg [7:0] qm_r;
    reg       qm8_r;
    reg [3:0] n1q_r;
    reg       de_r;
    reg [1:0] c_r;

    always @(posedge clk) begin
        qm_r  <= qm;
        qm8_r <= ~use_xnor;
        n1q_r <= popcnt8(qm);
        de_r  <= de;
        c_r   <= {c1, c0};
    end

    // ---------------- stage 2 ----------------
    reg  [4:0] cnt;                                   // two's complement running disparity
    wire       cnt_zero = (cnt == 5'd0);
    wire       cnt_neg  = cnt[4];
    wire       cnt_pos  = !cnt[4] && !cnt_zero;
    wire       n1_gt_n0 = (n1q_r > 4'd4);
    wire       n0_gt_n1 = (n1q_r < 4'd4);
    wire       n_equal  = (n1q_r == 4'd4);
    wire [4:0] n1x2     = {n1q_r, 1'b0};              // 2*N1 (0..16, mod 32)
    wire [4:0] n1_m_n0  = n1x2 - 5'd8;                // N1 - N0 = 2*N1 - 8
    wire [4:0] n0_m_n1  = 5'd8 - n1x2;                // N0 - N1

    always @(posedge clk) begin
        if (rst) begin
            cnt <= 5'd0;
            q   <= 10'b1101010100;
        end else if (!de_r) begin
            cnt <= 5'd0;
            case (c_r)
                2'b00:   q <= 10'b1101010100;
                2'b01:   q <= 10'b0010101011;
                2'b10:   q <= 10'b0101010100;
                default: q <= 10'b1010101011;
            endcase
        end else if (cnt_zero || n_equal) begin
            q   <= {~qm8_r, qm8_r, (qm8_r ? qm_r : ~qm_r)};
            cnt <= qm8_r ? (cnt + n1_m_n0) : (cnt + n0_m_n1);
        end else if ((cnt_pos && n1_gt_n0) || (cnt_neg && n0_gt_n1)) begin
            q   <= {1'b1, qm8_r, ~qm_r};
            cnt <= cnt + {3'd0, qm8_r, 1'b0} + n0_m_n1;
        end else begin
            q   <= {1'b0, qm8_r, qm_r};
            cnt <= cnt - {3'd0, ~qm8_r, 1'b0} + n1_m_n0;
        end
    end
endmodule
