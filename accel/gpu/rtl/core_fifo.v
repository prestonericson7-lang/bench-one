// core_fifo.v -- small synchronous FIFO (distributed RAM + registered output, FWFT style).
// dout/valid come straight from a register, so they are stable until popped (usable directly
// as an AXI W channel payload). Capacity = 2**AW + 1. Writing when full is a user error (the
// only user, core_writer, uses credit-based flow control that makes it impossible).
module core_fifo #(
    parameter W  = 64,
    parameter AW = 4
) (
    input  wire         clk,
    input  wire         rst,
    input  wire         wr,
    input  wire [W-1:0] din,
    input  wire         rd,        // pop the output word; only when valid
    output wire [W-1:0] dout,
    output wire         valid
);
    // 16 x 64: distributed RAM (Vivado would otherwise spend a whole RAMB36 on it)
    (* ram_style = "distributed" *)
    reg [W-1:0]  mem [0:(1<<AW)-1];
    reg [AW-1:0] wp, rp;
    reg [AW:0]   mcount;           // words held in mem (not counting the output register)
    reg [W-1:0]  q;
    reg          qv;

    wire load = (!qv || rd) && (mcount != 0);

    always @(posedge clk) begin
        if (wr)
            mem[wp] <= din;
    end

    always @(posedge clk) begin
        if (rst) begin
            wp     <= {AW{1'b0}};
            rp     <= {AW{1'b0}};
            mcount <= {(AW+1){1'b0}};
            qv     <= 1'b0;
        end else begin
            if (wr)
                wp <= wp + 1'b1;
            if (load) begin
                rp <= rp + 1'b1;
                q  <= mem[rp];
            end
            if (load)
                qv <= 1'b1;
            else if (rd)
                qv <= 1'b0;
            mcount <= mcount + {{AW{1'b0}}, wr} - {{AW{1'b0}}, load};
        end
    end

    assign dout  = q;
    assign valid = qv;
endmodule
