// tb_tmds.v -- tmds_encoder vs. the Python DVI 1.0 reference (sim/tmds_ref.py).
//
// Plusargs: +vec=<vector file from "tmds_ref.py gen">  +n=<vector count>  +dump=<output file>
// Drives one vector per clock, compares every symbol (latency 2) against the reference symbol,
// decodes every symbol back to data/control independently of the encoder, and tracks the
// running disparity of the emitted data stream (must equal a legal value, |disp| <= 8).
// The dumped symbols are re-checked by "tmds_ref.py check".
`timescale 1ns/1ps
module tb_tmds;
    localparam MAXV = 1 << 20;
    reg  [23:0] vec [0:MAXV-1];
    reg         clk = 0, rst = 1;
    reg  [7:0]  d = 0;
    reg         c0 = 0, c1 = 0, de = 0;
    wire [9:0]  q;

    tmds_encoder dut (.clk(clk), .rst(rst), .d(d), .c0(c0), .c1(c1), .de(de), .q(q));

    always #5 clk = ~clk;

    // ---- independent decoder ----
    function [9:0] decode;         // {is_ctrl, c1c0 or 0, data[7:0]}  (bit 9 = is_ctrl)
        input [9:0] s;
        reg [7:0] t, o;
        integer k;
        begin
            case (s)
                10'b1101010100: decode = {1'b1, 1'b0, 8'd0};
                10'b0010101011: decode = {1'b1, 1'b0, 8'd1};
                10'b0101010100: decode = {1'b1, 1'b0, 8'd2};
                10'b1010101011: decode = {1'b1, 1'b0, 8'd3};
                default: begin
                    t = s[9] ? ~s[7:0] : s[7:0];
                    o[0] = t[0];
                    for (k = 1; k < 8; k = k + 1)
                        o[k] = s[8] ? (t[k] ^ t[k-1]) : ~(t[k] ^ t[k-1]);
                    decode = {1'b0, 1'b0, o};
                end
            endcase
        end
    endfunction

    function integer ones10;
        input [9:0] s;
        integer k;
        begin
            ones10 = 0;
            for (k = 0; k < 10; k = k + 1) ones10 = ones10 + s[k];
        end
    endfunction

    integer n, i, fd, errs, disp, maxdisp, ndata, nctrl;
    reg [8*256-1:0] vecfile, dumpfile;
    reg [23:0] e;
    reg [9:0]  dec;

    initial begin
        if (!$value$plusargs("vec=%s", vecfile)) begin $display("need +vec="); $finish; end
        if (!$value$plusargs("dump=%s", dumpfile)) begin $display("need +dump="); $finish; end
        if (!$value$plusargs("n=%d", n)) begin $display("need +n="); $finish; end
        if (n > MAXV) begin $display("too many vectors"); $finish; end
        $readmemh(vecfile, vec, 0, n - 1);
        fd = $fopen(dumpfile, "w");
        errs = 0; disp = 0; maxdisp = 0; ndata = 0; nctrl = 0;

        // reset for a few clocks with garbage on the inputs
        repeat (4) begin
            @(negedge clk);
            d = $random; de = $random; c0 = $random; c1 = $random;
        end
        @(negedge clk);
        de = 0; c0 = 0; c1 = 0;        // stage 1 holds blanking when the reset is released
        @(negedge clk);
        if (q !== 10'b1101010100) begin
            $display("FAIL: reset output %b", q); errs = errs + 1;
        end
        rst = 0;
        // Vector i is driven after negedge i and sampled by stage 1 at the next posedge; at the
        // negedge after that, q holds the symbol of vector i-1 (2-clock latency).
        for (i = 0; i <= n; i = i + 1) begin
            if (i < n) begin
                e  = vec[i];
                de = e[20]; c1 = e[19]; c0 = e[18]; d = e[17:10];
            end else begin
                de = 0; c1 = 0; c0 = 0; d = 0;
            end
            @(negedge clk);
            if (i >= 1) begin
                e = vec[i-1];
                $fwrite(fd, "%03h\n", q);
                if (q !== e[9:0]) begin
                    errs = errs + 1;
                    if (errs <= 10)
                        $display("FAIL vec %0d: de=%b c=%b%b d=%02h q=%b exp=%b",
                                 i-1, e[20], e[19], e[18], e[17:10], q, e[9:0]);
                end
                dec = decode(q);
                if (e[20]) begin
                    ndata = ndata + 1;
                    if (dec[9] || dec[7:0] !== e[17:10]) begin
                        errs = errs + 1;
                        if (errs <= 10) $display("FAIL vec %0d: decoded %h sent %02h", i-1, dec, e[17:10]);
                    end
                    disp = disp + 2 * ones10(q) - 10;
                    if (disp > maxdisp) maxdisp = disp;
                    if (-disp > maxdisp) maxdisp = -disp;
                    if (disp > 8 || disp < -8) begin
                        errs = errs + 1;
                        if (errs <= 10) $display("FAIL vec %0d: running disparity %0d", i-1, disp);
                    end
                end else begin
                    nctrl = nctrl + 1;
                    disp = 0;
                    if (!dec[9] || dec[1:0] !== e[19:18]) begin
                        errs = errs + 1;
                        if (errs <= 10) $display("FAIL vec %0d: control decode %h sent %b", i-1, dec, e[19:18]);
                    end
                end
            end
        end
        $fclose(fd);
        $display("tb_tmds: %0d symbols (%0d data, %0d control) max|disparity|=%0d errors=%0d",
                 n, ndata, nctrl, maxdisp, errs);
        if (errs == 0) $display("tb_tmds: PASS"); else $display("tb_tmds: FAIL");
        $finish;
    end
endmodule
