/* ===========================================================================================
 *  hd_popcount.v -- count the set bits in W bits, in one clock, for any W
 * ===========================================================================================
 *
 *  This is the single operation the whole machine is built on. Similarity between two
 *  hypervectors is the number of bits that differ, so every query the system ever answers is
 *  XOR followed by this.
 *
 *  On the Cortex-M7 it is the expensive part. The M7 has no population count instruction, so the
 *  C falls back to a SWAR bit trick plus USAD8 and costs roughly 14 cycles per 32-bit word. An
 *  8192-bit compare is 256 words, so about 3,600 cycles, or six microseconds at 600 MHz.
 *
 *  In fabric it is free. Counting bits is a tree of adders, and a tree of adders is what an FPGA
 *  is made of. 512 bits collapse to a 10-bit total in one clock, and the depth grows with the
 *  logarithm of the width, so widening it costs almost nothing in latency.
 *
 *  WHY IT IS WRITTEN RECURSIVELY
 *  ------------------------------
 *  Splitting the input in half and instantiating two smaller copies produces a perfectly balanced
 *  adder tree at every width, without a table of hand-written cases. A flat behavioural sum leaves
 *  the tree shape to whatever the synthesiser feels like, and on a 7-series part that is usually a
 *  ripple chain deep enough to miss timing at any useful clock.
 *
 *  Verilog-2001 recursive module instantiation. Vivado and Icarus both accept it.
 * ===========================================================================================
 */

`default_nettype none

module hd_popcount #(
    parameter integer W = 512
) (
    input  wire [W-1:0]              d,
    output wire [$clog2(W+1)-1:0]    cnt
);

    generate
        if (W == 1) begin : leaf1
            assign cnt = d[0];
        end
        else if (W == 2) begin : leaf2
            assign cnt = d[0] + d[1];
        end
        else if (W == 3) begin : leaf3
            /* A 3-input case earns its place: it is exactly one full adder, which is one LUT6
             * plus its carry on a 7-series slice, and it stops the recursion one level earlier
             * than a 2-input base would. */
            assign cnt = d[0] + d[1] + d[2];
        end
        else begin : split
            localparam integer WL = W / 2;
            localparam integer WR = W - WL;

            wire [$clog2(WL+1)-1:0] cl;
            wire [$clog2(WR+1)-1:0] cr;

            hd_popcount #(.W(WL)) lo (.d(d[WL-1:0]),   .cnt(cl));
            hd_popcount #(.W(WR)) hi (.d(d[W-1:WL]),   .cnt(cr));

            /* Both halves are zero-extended to the parent's width before the add. The parent is
             * always at least one bit wider than either child, so this cannot overflow. */
            assign cnt = cl + cr;
        end
    endgenerate

endmodule

`default_nettype wire
