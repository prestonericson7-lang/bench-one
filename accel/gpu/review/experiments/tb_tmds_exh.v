// review/experiments/tb_tmds_exh.v -- drives rtl/tmds_encoder.v with the vectors of
// tmds_exhaustive.py (one vector per clock) and dumps q aligned to its vector (latency 2).
// Vector k is applied at negedge k; its symbol is on q from the 2nd following posedge, i.e. it
// is sampled at negedge k+2 (q only changes on posedges, so sampling at a negedge is race-free).
`timescale 1ns/1ps
module tb_tmds_exh;
    reg clk = 0;
    always #5 clk = ~clk;
    reg        rst = 1, de = 0, c0 = 0, c1 = 0;
    reg  [7:0] d = 0;
    wire [9:0] q;
    tmds_encoder dut (.clk(clk), .rst(rst), .d(d), .c0(c0), .c1(c1), .de(de), .q(q));

    integer fi, fo, r, n, k;
    reg [8*16-1:0] expstr;
    integer vr, vde, vc1, vc0, vd;
    initial begin
        fi = $fopen("tmds_vec.txt", "r");
        fo = $fopen("tmds_dut.txt", "w");
        n = 0;
        while (!$feof(fi)) begin
            r = $fscanf(fi, "%d %d %d %d %h %s\n", vr, vde, vc1, vc0, vd, expstr);
            if (r == 6) begin
                @(negedge clk);
                n = n + 1;
                if (n >= 3) $fwrite(fo, "%03x\n", q);   // symbol of vector n-2
                rst = vr[0]; de = vde[0]; c1 = vc1[0]; c0 = vc0[0]; d = vd[7:0];
            end
        end
        for (k = 0; k < 2; k = k + 1) begin
            @(negedge clk);
            $fwrite(fo, "%03x\n", q);
            rst = 0; de = 0;
        end
        $fclose(fi);
        $fclose(fo);
        $display("tb_tmds_exh: %0d vectors driven", n);
        $finish;
    end
endmodule
