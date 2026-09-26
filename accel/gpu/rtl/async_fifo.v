// async_fifo.v -- dual-clock FIFO with gray-code pointers, BRAM-inferable, FWFT read side.
//
// * Storage: 2**AW words of W bits in a simple dual-port RAM (write port on wclk, registered
//   read port with enable on rclk) -> Vivado infers block RAM (65 x 1024 = 2 x RAMB36).
//   The FWFT output register is the RAM's read register, so up to 2**AW + 1 words are held.
// * CDC: binary + gray pointers per side; each gray pointer is a register in its own domain
//   (one bit changes per increment) and crosses through a 2-FF synchroniser marked ASYNC_REG,
//   then is converted back to binary in a register (conservative: full/empty clear late).
//   Constraints for Vivado (see scanout.v header): the two clocks asynchronous
//   (set_clock_groups) plus set_bus_skew (<= one period of the faster clock) from
//   wgray_reg[*] to wgray_s1_reg[*] and from rgray_reg[*] to rgray_s1_reg[*].  wgray/rgray carry
//   KEEP: their MSB equals the binary MSB, and without KEEP synthesis merges gray_reg[AW] into
//   bin_reg[AW], so a constraint written on gray_reg[*] would silently miss that bit (seen in
//   Vivado 2026.1 OOC synthesis).
// * Write side: wr_en ignored when wfull.  wcount = words in the RAM as seen by the write
//   side (never lower than the true count, so it is safe for "space for N more" decisions).
// * Read side (FWFT): rvalid = rdata holds the head; rd_en pops it (ignored when !rvalid).
//   Sustains one pop per rclk.
// * Resets: wrst (sync to wclk) and rrst (sync to rclk) must be asserted together (system
//   start-up); resetting only one side while the other runs is not supported.
module async_fifo #(
    parameter W  = 65,
    parameter AW = 10
) (
    // write side
    input  wire          wclk,
    input  wire          wrst,
    input  wire          wr_en,
    input  wire [W-1:0]  wdata,
    output wire          wfull,
    output wire [AW:0]   wcount,
    // read side
    input  wire          rclk,
    input  wire          rrst,
    input  wire          rd_en,
    output wire [W-1:0]  rdata,
    output wire          rvalid
);
    localparam DEPTH = 1 << AW;

    // (No Verilog functions here: Verilator 4.038's V3Gate hits an internal error on a
    //  bin2gray function in the full gpu_top design; plain expressions/generate avoid it.)

    reg [W-1:0] mem [0:DEPTH-1];

    // ---------------- write domain ----------------
    reg  [AW:0] wbin;
    (* KEEP = "TRUE" *) reg [AW:0] wgray;        // CDC source: must stay its own register
    (* ASYNC_REG = "TRUE" *) reg [AW:0] rgray_s1;
    (* ASYNC_REG = "TRUE" *) reg [AW:0] rgray_s2;
    reg  [AW:0] rbin_w;                         // read pointer as seen by the write side
    wire [AW:0] rgray_s2_bin;                   // gray -> binary of the synchronised pointer

    assign wcount = wbin - rbin_w;
    assign wfull  = (wcount == {1'b1, {AW{1'b0}}});     // == DEPTH
    wire   do_wr  = wr_en && !wfull;
    wire [AW:0] wbin_nx = wbin + 1'b1;

    always @(posedge wclk) begin
        if (do_wr)
            mem[wbin[AW-1:0]] <= wdata;
    end

    always @(posedge wclk) begin
        if (wrst) begin
            wbin     <= {(AW+1){1'b0}};
            wgray    <= {(AW+1){1'b0}};
            rgray_s1 <= {(AW+1){1'b0}};
            rgray_s2 <= {(AW+1){1'b0}};
            rbin_w   <= {(AW+1){1'b0}};
        end else begin
            if (do_wr) begin
                wbin  <= wbin_nx;
                wgray <= wbin_nx ^ (wbin_nx >> 1);
            end
            rgray_s1 <= rgray;
            rgray_s2 <= rgray_s1;
            rbin_w   <= rgray_s2_bin;
        end
    end

    // ---------------- read domain ----------------
    reg  [AW:0] rbin;
    (* KEEP = "TRUE" *) reg [AW:0] rgray;        // CDC source: must stay its own register
    (* ASYNC_REG = "TRUE" *) reg [AW:0] wgray_s1;
    (* ASYNC_REG = "TRUE" *) reg [AW:0] wgray_s2;
    reg  [AW:0] wbin_r;                         // write pointer as seen by the read side
    wire [AW:0] wgray_s2_bin;
    reg  [W-1:0] q;
    reg          q_valid;

    wire        ram_empty = (rbin == wbin_r);
    wire        ram_rd    = !ram_empty && (!q_valid || rd_en);
    wire [AW:0] rbin_nx   = rbin + 1'b1;

    always @(posedge rclk) begin                // RAM read register (no reset: BRAM DOUT reg)
        if (ram_rd)
            q <= mem[rbin[AW-1:0]];
    end

    always @(posedge rclk) begin
        if (rrst) begin
            rbin     <= {(AW+1){1'b0}};
            rgray    <= {(AW+1){1'b0}};
            wgray_s1 <= {(AW+1){1'b0}};
            wgray_s2 <= {(AW+1){1'b0}};
            wbin_r   <= {(AW+1){1'b0}};
            q_valid  <= 1'b0;
        end else begin
            if (ram_rd) begin
                rbin    <= rbin_nx;
                rgray   <= rbin_nx ^ (rbin_nx >> 1);
                q_valid <= 1'b1;
            end else if (rd_en) begin
                q_valid <= 1'b0;
            end
            wgray_s1 <= wgray;
            wgray_s2 <= wgray_s1;
            wbin_r   <= wgray_s2_bin;
        end
    end

    // gray -> binary: b[AW] = g[AW], b[k] = b[k+1] ^ g[k]
    genvar gi;
    generate
        for (gi = 0; gi <= AW; gi = gi + 1) begin : g_g2b
            assign rgray_s2_bin[gi] = ^(rgray_s2 >> gi);
            assign wgray_s2_bin[gi] = ^(wgray_s2 >> gi);
        end
    endgenerate

    assign rdata  = q;
    assign rvalid = q_valid;
endmodule
