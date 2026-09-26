// review/experiments/tb_par_rx_glitch.v -- what a short STROBE glitch does to rtl/par_rx.v
// (and to the filtered proposal review/experiments/par_rx_filt.v, driven by the same pins).
//
// Teensy model: SPEC 3 transfer timing (setup/hold in ns), low half first, SOR on transfer 0.
// Per trial: reset, record A (24 words) with ONE glitch injected, then a clean record B.
//   glitch kind 0 "ringback":  G ns after a STROBE edge, STROBE returns to the old level for
//                              W ns, then back (reflection / ringing on the cable)
//   glitch kind 1 "crosstalk": at the moment D changes (start of a transfer, i.e. >= hold time
//                              after the previous edge) STROBE pulses away from its level for
//                              W ns and back (16 D lines switching next to it on the cable)
// Pass criterion per trial: the pushed word stream equals A then B exactly (data + sor flag).
// Plusargs: +half=<core half period ns*1000> (default 3361 = 148.75 MHz), +seed=N
`timescale 1ns/1ps
module tb_par_rx_glitch;
    reg clk = 0;
    real half;
    integer half_ps;
    initial begin
        if (!$value$plusargs("half=%d", half_ps)) half_ps = 3361;
        half = half_ps / 1000.0;
    end
    always #(half) clk = ~clk;

    reg        rst = 1;
    reg [15:0] pd = 0;
    reg        psor = 0, pstb = 0;

    wire        busy_a, busy_b, wr_a, wr_b, act_a, act_b;
    wire [32:0] din_a, din_b;
    wire [31:0] wx_a, wx_b;

    par_rx      dut_a (.clk(clk), .rst(rst), .pin_d(pd), .pin_sor(psor), .pin_strobe(pstb),
                       .pin_busy(busy_a), .f_wr(wr_a), .f_din(din_a), .f_free(11'd1024),
                       .words_rx(wx_a), .active(act_a));
    par_rx_filt dut_b (.clk(clk), .rst(rst), .pin_d(pd), .pin_sor(psor), .pin_strobe(pstb),
                       .pin_busy(busy_b), .f_wr(wr_b), .f_din(din_b), .f_free(11'd1024),
                       .words_rx(wx_b), .active(act_b));

    // ---------------------------------------------------------------- capture
    reg [32:0] got_a [0:255];
    reg [32:0] got_b [0:255];
    integer na = 0, nb = 0;
    always @(posedge clk) begin
        if (wr_a && na < 256) begin got_a[na] <= din_a; na <= na + 1; end
        if (wr_b && nb < 256) begin got_b[nb] <= din_b; nb <= nb + 1; end
    end

    // ---------------------------------------------------------------- stimulus
    reg [32:0] exp_w [0:47];
    integer seed;
    real setup_ns, hold_ns;
    integer gl_xfer, gl_kind;
    real gl_delay, gl_width;

    task xfer(input [15:0] dv, input sv, input integer idx);
        begin
            pd = dv; psor = sv;
            if (idx == gl_xfer && gl_kind == 1) begin
                // crosstalk: STROBE pulses away and back while all D lines switch
                pstb = ~pstb; #(gl_width); pstb = ~pstb;
                #(setup_ns - gl_width);
            end else begin
                #(setup_ns);
            end
            pstb = ~pstb;
            if (idx == gl_xfer && gl_kind == 0) begin
                #(gl_delay);  pstb = ~pstb;       // ringback to the old level
                #(gl_width);  pstb = ~pstb;       // and back
                #(hold_ns - gl_delay - gl_width);
            end else begin
                #(hold_ns);
            end
        end
    endtask

    task send_record(input integer base, input integer glitch_on);
        integer i;
        reg [31:0] w;
        begin
            for (i = 0; i < 24; i = i + 1) begin
                w = {base[7:0], 8'hA5, i[7:0], 8'h5A} ^ (i * 32'h01010101);
                if (i == 0) w = {4'd1, w[27:0]};            // TRI type
                exp_w[(base & 1) * 24 + i] = {(i == 0) ? 1'b1 : 1'b0, w};
                xfer(w[15:0],  i == 0, glitch_on ? 2 * i     : -2);
                xfer(w[31:16], 1'b0,   glitch_on ? 2 * i + 1 : -2);
            end
        end
    endtask

    integer trial, wi, kind, bad_a, bad_b, i, ok_a, ok_b;
    integer ntr, fm;
    real widths [0:7];
    integer save_xfer;

    task one_trial(input integer k, input real w, output integer oka, output integer okb);
        begin
            rst = 1;
            repeat (4) @(posedge clk);
            #(($random(seed) & 1023) / 1024.0 * 2.0 * half);    // random phase vs the clock
            rst = 0;
            repeat (2) @(posedge clk);
            @(negedge clk) begin na = 0; nb = 0; end
            #(($random(seed) & 1023) / 1024.0 * 2.0 * half);
            gl_kind  = k;
            gl_width = w;
            gl_delay = 0.5 + (($random(seed) & 1023) / 1024.0) * 2.0;
            save_xfer = ($random(seed) & 32'h7fffffff) % 46 + 1;   // not the very first/last
            gl_xfer  = save_xfer;
            send_record(2, 1);
            gl_xfer  = -1;
            send_record(3, 0);
            repeat (8) @(posedge clk);
            oka = (na == 48); okb = (nb == 48);
            for (i = 0; i < 48; i = i + 1) begin
                if (i < na && got_a[i] !== exp_w[i]) oka = 0;
                if (i < nb && got_b[i] !== exp_w[i]) okb = 0;
            end
        end
    endtask

    initial begin
        if (!$value$plusargs("seed=%d", seed)) seed = 1;
        widths[0] = 1.0; widths[1] = 2.0; widths[2] = 3.0; widths[3] = 4.0;
        widths[4] = 5.0; widths[5] = 6.0; widths[6] = 8.0; widths[7] = 12.0;
        ntr = 40;
        setup_ns = 30.0; hold_ns = 60.0;
        gl_xfer = -1;

        // ---- 0: clean traffic at the SPEC minimum timing, both receivers must be exact
        ok_a = 0; ok_b = 0;
        for (trial = 0; trial < 50; trial = trial + 1) begin
            gl_xfer = -1;
            rst = 1; repeat (4) @(posedge clk);
            #(($random(seed) & 1023) / 1024.0 * 2.0 * half);
            rst = 0; repeat (2) @(posedge clk);
            @(negedge clk) begin na = 0; nb = 0; end
            #(($random(seed) & 1023) / 1024.0 * 2.0 * half);
            send_record(2, 0); send_record(3, 0);
            repeat (8) @(posedge clk);
            bad_a = (na != 48); bad_b = (nb != 48);
            for (i = 0; i < 48; i = i + 1) begin
                if (got_a[i] !== exp_w[i]) bad_a = 1;
                if (got_b[i] !== exp_w[i]) bad_b = 1;
            end
            if (!bad_a) ok_a = ok_a + 1;
            if (!bad_b) ok_b = ok_b + 1;
        end
        $display("clean 30/60 ns, core half %0.3f ns: rtl/par_rx %0d/50 exact, par_rx_filt %0d/50 exact",
                 half, ok_a, ok_b);

        // ---- 1: glitch sweep
        for (kind = 0; kind < 2; kind = kind + 1) begin
            for (wi = 0; wi < 8; wi = wi + 1) begin
                bad_a = 0; bad_b = 0;
                for (trial = 0; trial < ntr; trial = trial + 1) begin
                    one_trial(kind, widths[wi], ok_a, ok_b);
                    if (!ok_a) begin
                        bad_a = bad_a + 1;
                        if (bad_a == 1) begin
                            fm = -1;
                            for (i = 47; i >= 0; i = i - 1)
                                if (i >= na || got_a[i] !== exp_w[i]) fm = i;
                            $display("  example (%s %0.1f ns at transfer %0d): rtl/par_rx pushed %0d words (48 expected); first bad word %0d: got %h expected %h; next got %h",
                                     kind ? "crosstalk" : "ringback", widths[wi], save_xfer, na,
                                     fm, got_a[fm], exp_w[fm], got_a[fm + 1]);
                        end
                    end
                    if (!ok_b) bad_b = bad_b + 1;
                end
                $display("%-9s glitch %5.1f ns: corrupted trials  rtl/par_rx %2d/%0d   par_rx_filt %2d/%0d",
                         kind ? "crosstalk" : "ringback", widths[wi], bad_a, ntr, bad_b, ntr);
            end
        end
        $finish;
    end
endmodule
