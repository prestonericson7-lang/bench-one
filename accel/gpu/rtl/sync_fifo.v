// ---------------------------------------------------------------------------------------------
// sync_fifo -- first-word-fall-through synchronous FIFO, BRAM-inferable (rtl/INTERFACES.md)
//
//   depth = 2**AW entries of W bits. Used for the Teensy command FIFO (33 x 1024) and the PS
//   command FIFO (33 x 512).
//
//   * wr is ignored when full, rd is ignored when empty.
//   * dout is valid whenever !empty (FWFT). rd pops the word shown on dout.
//   * count = used entries (including a word that is still moving from the RAM to the output
//     register), free = 2**AW - count. count/free/full are registered.
//   * Write-to-visible latency: a word written into an empty FIFO appears on dout 2 cycles
//     after the write (RAM write, then RAM read into the output register). During that single
//     cycle empty = 1 while count = 1.
//
// Structure: a simple-dual-port RAM with a synchronous, enabled read (maps onto the BRAM output
// latch) plus the valid flag of that output register. The RAM is only read when it holds at
// least one entry, and it can never hold 2**AW entries while being read (see below), so the read
// and write addresses never collide -> no read-during-write ambiguity.
//   ram_cnt = entries in the RAM not yet moved to the output register
//   cnt     = ram_cnt + ovalid (total), full = (cnt == 2**AW)
//   ovalid = 0 with ram_cnt > 0 only lasts one cycle (the fetch happens immediately), so
//   ram_cnt == 2**AW (rptr == wptr with data) is unreachable.
// ---------------------------------------------------------------------------------------------
module sync_fifo #(
    parameter W  = 33,
    parameter AW = 10
) (
    input  wire          clk,
    input  wire          rst,
    input  wire          wr,
    input  wire [W-1:0]  din,
    input  wire          rd,
    output wire [W-1:0]  dout,
    output wire          empty,
    output wire          full,
    output wire [AW:0]   count,
    output wire [AW:0]   free
);
    localparam [AW:0] DEPTH = {1'b1, {AW{1'b0}}};   // 2**AW
    localparam [AW:0] ONE   = {{AW{1'b0}}, 1'b1};

    reg [W-1:0]  mem [0:(1<<AW)-1];
    reg [W-1:0]  oreg;                // RAM read register = output register
    reg          ovalid;
    reg [AW-1:0] wptr, rptr;
    reg [AW:0]   ram_cnt;
    reg [AW:0]   cnt;
    reg [AW:0]   free_r;
    reg          full_r;

    wire do_wr = wr && !full_r;
    wire do_rd = rd && ovalid;
    wire fetch = (ram_cnt != {(AW+1){1'b0}}) && (!ovalid || do_rd);

    wire [AW:0] ram_cnt_nx = ram_cnt + (do_wr ? ONE : {(AW+1){1'b0}}) - (fetch ? ONE : {(AW+1){1'b0}});
    wire [AW:0] cnt_nx     = cnt     + (do_wr ? ONE : {(AW+1){1'b0}}) - (do_rd ? ONE : {(AW+1){1'b0}});

    // RAM write port
    always @(posedge clk) begin
        if (do_wr)
            mem[wptr] <= din;
    end

    // RAM read port (synchronous, enabled) -> output register. No reset (BRAM output latch).
    always @(posedge clk) begin
        if (fetch)
            oreg <= mem[rptr];
    end

    always @(posedge clk) begin
        if (rst) begin
            wptr    <= {AW{1'b0}};
            rptr    <= {AW{1'b0}};
            ram_cnt <= {(AW+1){1'b0}};
            cnt     <= {(AW+1){1'b0}};
            free_r  <= DEPTH;
            full_r  <= 1'b0;
            ovalid  <= 1'b0;
        end else begin
            if (do_wr) wptr <= wptr + 1'b1;
            if (fetch) rptr <= rptr + 1'b1;
            ram_cnt <= ram_cnt_nx;
            cnt     <= cnt_nx;
            free_r  <= DEPTH - cnt_nx;
            full_r  <= (cnt_nx == DEPTH);
            if (fetch)
                ovalid <= 1'b1;
            else if (do_rd)
                ovalid <= 1'b0;
        end
    end

    assign dout  = oreg;
    assign empty = !ovalid;
    assign full  = full_r;
    assign count = cnt;
    assign free  = free_r;

endmodule
