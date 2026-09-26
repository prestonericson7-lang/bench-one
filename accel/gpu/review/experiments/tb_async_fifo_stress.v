// review/experiments/tb_async_fifo_stress.v -- independent random stress of rtl/async_fifo.v
// (AW = 4 so full/empty/wrap are hit constantly) for several write/read clock ratios.
// Checks: data order/integrity (every popped word == next written word), no write accepted
// when wfull, wcount >= words physically in the RAM (safe for the scanout reservation), and
// the FWFT output never changes while rvalid && !rd_en.
// Plusargs: +wps=<write half period ps> +rps=<read half period ps> +seed=N +n=<words>
`timescale 1ns/1ps
module tb_async_fifo_stress;
    localparam W = 65, AW = 4;
    integer wps, rps, seed, nwords;
    reg wclk = 0, rclk = 0;
    reg wrst = 1, rrst = 1;
    initial begin
        if (!$value$plusargs("wps=%d", wps)) wps = 3361;
        if (!$value$plusargs("rps=%d", rps)) rps = 6723;
        if (!$value$plusargs("seed=%d", seed)) seed = 1;
        if (!$value$plusargs("n=%d", nwords)) nwords = 200000;
    end
    always #(wps / 1000.0) wclk = ~wclk;
    always #(rps / 1000.0) rclk = ~rclk;

    reg          wr_en = 0, rd_en = 0;
    reg  [W-1:0] wdata = 0;
    wire         wfull, rvalid;
    wire [AW:0]  wcount;
    wire [W-1:0] rdata;

    async_fifo #(.W(W), .AW(AW)) dut (
        .wclk(wclk), .wrst(wrst), .wr_en(wr_en), .wdata(wdata), .wfull(wfull), .wcount(wcount),
        .rclk(rclk), .rrst(rrst), .rd_en(rd_en), .rdata(rdata), .rvalid(rvalid));

    function [W-1:0] pat;
        input integer k;
        begin
            pat = {k[0], k[31:0] * 32'h9E3779B1, k[31:0]};
        end
    endfunction

    integer nwr = 0, nrd = 0, errs = 0, maxc = 0, wpct, rpct;
    // write side
    always @(posedge wclk) begin
        if (!wrst) begin
            if (wr_en && !wfull) nwr <= nwr + 1;
            if (wr_en && wfull)  ; // ignored by design
        end
    end
    always @(negedge wclk) begin
        if (!wrst && nwr < nwords) begin
            wr_en = (($random(seed) & 255) < wpct);
            wdata = pat(nwr);
        end else begin
            wr_en = 0;
        end
    end
    // RAM occupancy seen from the write side must never be under-estimated by wcount:
    // words in RAM <= nwr - (words moved into the output register) <= wcount
    integer moved = 0;
    always @(posedge rclk) if (!rrst && dut.ram_rd) moved <= moved + 1;
    always @(posedge wclk) begin
        if (!wrst) begin
            if (wcount > (1 << AW)) begin errs = errs + 1; $display("ERROR wcount %0d > depth", wcount); end
            if (nwr - moved > wcount) begin
                errs = errs + 1;
                if (errs < 5) $display("ERROR wcount %0d under-estimates RAM occupancy %0d", wcount, nwr - moved);
            end
            if (wcount > maxc) maxc = wcount;
        end
    end
    // read side
    reg [W-1:0] held; reg held_v = 0;
    always @(posedge rclk) begin
        if (!rrst) begin
            if (held_v && rvalid && rdata !== held) begin
                errs = errs + 1; $display("ERROR FWFT head changed without a pop");
            end
            if (rvalid && rd_en) begin
                if (rdata !== pat(nrd)) begin
                    errs = errs + 1;
                    if (errs < 5) $display("ERROR word %0d: got %h expected %h", nrd, rdata, pat(nrd));
                end
                nrd <= nrd + 1;
                held_v <= 0;
            end else begin
                held_v <= rvalid; held <= rdata;
            end
        end
    end
    always @(negedge rclk) rd_en = !rrst && (($random(seed) & 255) < rpct);

    integer phase;
    initial begin
        wpct = 200; rpct = 200;
        #50 wrst = 0;
        #37 rrst = 0;                 // released at different times (as clkgen does)
        for (phase = 0; phase < 4; phase = phase + 1) begin
            case (phase)
                0: begin wpct = 250; rpct = 60;  end   // mostly full
                1: begin wpct = 40;  rpct = 250; end   // mostly empty
                2: begin wpct = 180; rpct = 180; end
                3: begin wpct = 256; rpct = 256; end   // streaming both sides
            endcase
            #((nwords / 4) * 2 * (wps > rps ? wps : rps) / 1000.0 * 3);
        end
        wpct = 0; rpct = 256;
        #(20000);
        $display("async_fifo stress w=%0d ps r=%0d ps: written %0d read %0d max wcount %0d errors %0d -> %s",
                 2 * wps, 2 * rps, nwr, nrd, maxc, errs, (errs == 0 && nrd == nwr && nwr > 1000) ? "PASS" : "FAIL");
        $finish;
    end
endmodule
