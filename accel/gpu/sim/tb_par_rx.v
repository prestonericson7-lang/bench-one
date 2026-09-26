`timescale 1ns / 1ps
// tb_par_rx -- rtl/par_rx.v + rtl/sync_fifo.v (Teensy FIFO 33 x 1024) against a Teensy bus model.
//
// Teensy model (SPEC 3): drives D[15:0]/SOR, waits setup, toggles STROBE, waits hold; low half
// first, SOR on the first transfer of a record; waits for BUSY = 0 before each record, then sends
// the record without looking at BUSY. Timing is real time (ns) with random setup 30-60 ns and
// hold 60-120 ns (plus exact-minimum and beyond-spec margin phases); the core clock runs at
// 148.75 MHz (T = 6.7227 ns) with a random start phase, so edges fall at arbitrary points of
// the clock period. A transfer-level reference model of the half-word assembler predicts every
// pushed word.
//
// Phases:  A random stream, fast consumer          B exact 30/60 ns timing
//          C slow consumer -> FIFO fills, BUSY throttles the Teensy (no overflow allowed)
//          D mid-record SOR resync, aborted records, sor=0 garbage runs, random consumer
//          E reset with a dangling half word (discarded), words_rx restarts
//          F 'active' timeout (ACTIVE_CYCLES shortened to 3000) and re-trigger
//          G beyond-spec margin: setup 12-20 ns, hold 25-40 ns
//          H sampling-window check (simulation-only construct, far outside the bus spec):
//            setup 0 (D/SOR change in the same time step as STROBE), hold 22-28 ns. Every word
//            is right only if D/SOR are sampled after the STROBE edge and <= 22 ns after it,
//            and a STROBE level of 22 ns (> 3 T) is still accepted by the glitch filter.
//          I STROBE glitches (review R1-01), SPEC timing, about half of all transfers carry one
//            glitch pulse of 0.3-13.0 ns (< 2 T): crosstalk (STROBE pulses away and back when
//            D changes), ringback (0.2-3 ns after the real edge STROBE returns to the old level
//            for the pulse width) or a pulse in the middle of the hold time. The expected word
//            stream is unchanged: a glitch must never add, drop or corrupt a word.
// Checks: every f_din word and SOR flag (at push and again at the FIFO output), no push while the
//   FIFO is full, pin_busy == registered (rst || free < 64) every cycle, BUSY = 1 in reset,
//   words_rx, active.
// Plusargs: +seed=N. Prints "PASS tb_par_rx" or "FAIL tb_par_rx".
module tb_par_rx;
    localparam real TCLK = 1000.0 / 148.75;       // 6.722689 ns
    localparam      ACT  = 3000;

    // ---------------------------------------------------------------- clock / reset
    reg clk = 1'b0;
    reg rst = 1'b1;
    integer seed = 1;
    integer seed0 = 1;
    real    phase0;

    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        seed0 = seed;
        phase0 = TCLK * (($random(seed) & 32'hffff) / 65536.0);
        #(phase0);
        forever #(TCLK / 2.0) clk = ~clk;
    end

    // ---------------------------------------------------------------- DUT
    reg  [15:0] tb_d = 16'd0;
    reg         tb_sor = 1'b0;
    reg         tb_strobe = 1'b0;
    wire        busy;
    wire        f_wr;
    wire [32:0] f_din;
    wire [10:0] f_free, f_count;
    wire [31:0] words_rx;
    wire        active;
    reg         rd = 1'b0;
    wire [32:0] dout;
    wire        empty, full;

    par_rx #(.ACTIVE_CYCLES(ACT)) dut (
        .clk(clk), .rst(rst), .pin_d(tb_d), .pin_sor(tb_sor), .pin_strobe(tb_strobe),
        .pin_busy(busy), .f_wr(f_wr), .f_din(f_din), .f_free(f_free),
        .words_rx(words_rx), .active(active)
    );

    sync_fifo #(.W(33), .AW(10)) fifo (
        .clk(clk), .rst(rst), .wr(f_wr), .din(f_din), .rd(rd),
        .dout(dout), .empty(empty), .full(full), .count(f_count), .free(f_free)
    );

    // ---------------------------------------------------------------- errors
    integer errors = 0;
    task err;
        input [8*96-1:0] msg;
        begin
            errors = errors + 1;
            if (errors <= 20) $display("ERROR %t: %0s", $time, msg);
        end
    endtask

    // ---------------------------------------------------------------- reference model
    localparam QN = 65536;
    reg [32:0] expq [0:QN-1];
    integer et = 0, ph = 0, ch = 0;      // tail, push-check head, consume-check head
    reg        r_phase = 1'b0;
    reg [15:0] r_lo = 16'd0;
    reg        r_sor = 1'b0;
    integer    exp_words = 0;            // words expected since the last reset

    task ref_xfer;
        input [15:0] d;
        input        s;
        begin
            if (s || !r_phase) begin
                r_lo = d; r_sor = s; r_phase = 1'b1;
            end else begin
                expq[et % QN] = {r_sor, d, r_lo};
                et = et + 1;
                r_phase = 1'b0;
                exp_words = exp_words + 1;
            end
        end
    endtask

    // ---------------------------------------------------------------- Teensy model
    real su_min = 30.0, su_max = 60.0, ho_min = 60.0, ho_max = 120.0;
    integer n_xfers = 0, n_records = 0, n_busy_waits = 0;

    function real rnd01;
        input dummy;
        begin
            rnd01 = ($random(seed) & 32'h7fffffff) / 2147483647.0;
        end
    endfunction

    // glitch injection (phase I): gl_pct % of the transfers get one glitch pulse
    integer gl_pct = 0;
    integer n_gl_xtalk = 0, n_gl_ring = 0, n_gl_mid = 0;
    real    gl_wmin = 0.3, gl_wmax = 13.0;   // pulse widths, all < 2 T = 13.445 ns

    task xfer;
        input [15:0] d;
        input        s;
        real tsu, tho, gw, gd;
        integer kind;
        begin
            tsu = su_min + (su_max - su_min) * rnd01(0);
            tho = ho_min + (ho_max - ho_min) * rnd01(0);
            kind = -1;
            if (gl_pct > 0 && (($random(seed) & 32'h7fffffff) % 100) < gl_pct)
                kind = ($random(seed) & 32'h7fffffff) % 3;
            gw = gl_wmin + (gl_wmax - gl_wmin) * rnd01(0);
            tb_d = d; tb_sor = s;
            if (kind == 0) begin
                // crosstalk: STROBE pulses away and back while D/SOR switch
                tb_strobe = ~tb_strobe; #(gw); tb_strobe = ~tb_strobe;
                n_gl_xtalk = n_gl_xtalk + 1;
                #(tsu - gw);
            end else begin
                #(tsu);
            end
            ref_xfer(d, s);                // before the DUT can possibly see the edge
            tb_strobe = ~tb_strobe;
            n_xfers = n_xfers + 1;
            if (kind == 1) begin
                // ringback: back to the old level for gw ns, gd ns after the real edge
                gd = 0.2 + 2.8 * rnd01(0);
                #(gd); tb_strobe = ~tb_strobe; #(gw); tb_strobe = ~tb_strobe;
                n_gl_ring = n_gl_ring + 1;
                #(tho - gd - gw);
            end else if (kind == 2) begin
                // pulse somewhere in the middle of the hold time
                gd = tho / 3.0 + (tho / 3.0) * rnd01(0);
                #(gd); tb_strobe = ~tb_strobe; #(gw); tb_strobe = ~tb_strobe;
                n_gl_mid = n_gl_mid + 1;
                #(tho - gd - gw);
            end else begin
                #(tho);
            end
        end
    endtask

    task wait_not_busy;
        begin
            while (busy) begin
                n_busy_waits = n_busy_waits + 1;
                #(50.0 + 400.0 * rnd01(0));
            end
        end
    endtask

    // send the first nhalves transfers of a random record (48 = complete record)
    task send_record;
        input integer nhalves;
        integer i;
        reg [31:0] w;
        begin
            wait_not_busy;
            w = 32'd0;
            for (i = 0; i < nhalves; i = i + 1) begin
                if (i % 2 == 0) begin
                    w = $random(seed);
                    xfer(w[15:0], i == 0);
                end else begin
                    xfer(w[31:16], 1'b0);
                end
            end
            n_records = n_records + 1;
        end
    endtask

    // sor=0 transfers with no record open (collector garbage; par_rx just assembles them)
    task send_garbage;
        input integer nhalves;
        integer i;
        begin
            wait_not_busy;
            for (i = 0; i < nhalves; i = i + 1)
                xfer($random(seed), 1'b0);
        end
    endtask

    // ---------------------------------------------------------------- consumer + monitors
    integer pop_pct = 100;
    integer n_pushed = 0, n_popped = 0, n_busy_cycles = 0, max_count = 0;
    reg     exp_busy = 1'b1;

    always @(negedge clk)
        rd <= !empty && ((($random(seed) & 32'h7fffffff) % 100) < pop_pct);

    always @(posedge clk) begin
        // BUSY: registered (rst || free < 64), compared one cycle later
        if (busy !== exp_busy) err("pin_busy != registered (rst || f_free < 64)");
        exp_busy <= rst || (f_free < 11'd64);
        if (busy) n_busy_cycles = n_busy_cycles + 1;
        if (f_count > max_count) max_count = f_count;
        if (!rst) begin
            if (f_wr) begin
                if (full) err("push while the Teensy FIFO is full (word lost)");
                if (ph >= et) err("unexpected push (nothing expected)");
                else begin
                    if (f_din !== expq[ph % QN]) begin
                        err("pushed word mismatch");
                        $display("       got %h expected %h", f_din, expq[ph % QN]);
                    end
                    ph = ph + 1;
                end
                n_pushed = n_pushed + 1;
            end
            if (rd && !empty) begin
                if (ch >= et) err("unexpected FIFO output");
                else begin
                    if (dout !== expq[ch % QN]) err("FIFO output word mismatch");
                    ch = ch + 1;
                end
                n_popped = n_popped + 1;
            end
        end
    end

    // ---------------------------------------------------------------- helpers
    task wait_drained;
        integer guard;
        begin
            guard = 0;
            #(200.0);
            while ((ch < et || !empty) && guard < 2000000) begin
                @(posedge clk); guard = guard + 1;
            end
            if (guard >= 2000000) err("timeout waiting for the FIFO to drain");
            repeat (4) @(posedge clk);
        end
    endtask

    task check_counts;
        input [8*16-1:0] tag;
        begin
            if (ph != et) err("not every expected word was pushed");
            if (words_rx !== exp_words) begin
                err("words_rx mismatch");
                $display("       %0s: words_rx=%0d expected %0d", tag, words_rx, exp_words);
            end
        end
    endtask

    task do_reset;
        input integer ncycles;
        begin
            @(negedge clk); rst = 1'b1;
            repeat (ncycles) @(negedge clk);
            // reset flushes the FIFO and the half-word phase
            ph = et; ch = et; r_phase = 1'b0; exp_words = 0;
            rst = 1'b0;
        end
    endtask

    // ---------------------------------------------------------------- test sequence
    integer k, n, sel, busy_waits_before;

    initial begin
        #1;
        if (busy !== 1'b1) err("BUSY not 1 at power-up");
        repeat (10) @(negedge clk);
        if (busy !== 1'b1) err("BUSY not 1 during reset");
        rst = 1'b0;
        repeat (5) @(negedge clk);
        if (busy !== 1'b0) err("BUSY not 0 after reset with an empty FIFO");

        // ---- A: long random stream, fast consumer
        pop_pct = 100;
        for (k = 0; k < 300; k = k + 1) send_record(48);
        wait_drained; check_counts("A");
        $display("INFO phase A: %0d records, %0d words", n_records, n_pushed);

        // ---- B: exact minimum spec timing
        su_min = 30.0; su_max = 30.0; ho_min = 60.0; ho_max = 60.0;
        for (k = 0; k < 150; k = k + 1) send_record(48);
        wait_drained; check_counts("B");
        su_min = 30.0; su_max = 60.0; ho_min = 60.0; ho_max = 120.0;

        // ---- C: slow consumer, BUSY must throttle, no overflow
        pop_pct = 2;
        busy_waits_before = n_busy_waits;
        for (k = 0; k < 250; k = k + 1) send_record(48);
        pop_pct = 100;
        wait_drained; check_counts("C");
        $display("INFO phase C: max FIFO level %0d, Teensy BUSY polls %0d, BUSY cycles %0d",
                 max_count, n_busy_waits - busy_waits_before, n_busy_cycles);
        if (n_busy_waits == busy_waits_before) err("phase C never saw BUSY = 1");
        if (max_count < 1024 - 64) err("phase C did not fill the FIFO up to the BUSY threshold");

        // ---- D: resync / aborted records / garbage, random consumer
        pop_pct = 50;
        for (k = 0; k < 400; k = k + 1) begin
            sel = ($random(seed) & 32'h7fffffff) % 8;
            if (sel < 4)       send_record(48);
            else if (sel < 7)  send_record(1 + (($random(seed) & 32'h7fffffff) % 47)); // aborted
            else               send_garbage(1 + (($random(seed) & 32'h7fffffff) % 7));
        end
        send_record(48);
        pop_pct = 100;
        wait_drained; check_counts("D");

        // ---- E: reset with a dangling low half
        send_record(1);                   // SOR low half only
        #(300.0);
        do_reset(3);
        repeat (5) @(negedge clk);
        if (words_rx !== 32'd0) err("words_rx not cleared by reset");
        send_record(48);
        wait_drained; check_counts("E");
        if (words_rx !== 32'd24) err("words_rx != 24 after one record following reset");

        // ---- F: active timeout / retrigger
        if (active !== 1'b1) err("active not 1 right after traffic");
        repeat (ACT + 10) @(posedge clk);
        if (active !== 1'b0) err("active still 1 after ACTIVE_CYCLES of silence");
        xfer(16'h1234, 1'b1);
        repeat (8) @(posedge clk);
        if (active !== 1'b1) err("active not re-armed by a strobe edge");
        xfer(16'h5678, 1'b0);
        wait_drained; check_counts("F");

        // ---- G: beyond-spec timing margin (setup 12-20 ns, hold 25-40 ns)
        su_min = 12.0; su_max = 20.0; ho_min = 25.0; ho_max = 40.0;
        pop_pct = 100;
        for (k = 0; k < 150; k = k + 1) send_record(48);
        wait_drained; check_counts("G");

        // ---- H: sampling-window check (simulation only, far outside the bus spec):
        // D/SOR change in the same time step as STROBE (setup 0) and change again 22-28 ns
        // later. The filtered receiver samples D/SOR 2..3 T (13.4-20.2 ns) after the edge, so
        // it gets every word right; sampling before the edge or later than 22 ns would not, and
        // a filter needing more than 3 samples would drop transfers.
        su_min = 0.0; su_max = 0.0; ho_min = 22.0; ho_max = 28.0;
        for (k = 0; k < 150; k = k + 1) send_record(48);
        wait_drained; check_counts("H");

        // ---- I: STROBE glitches at SPEC timing, random consumer (BUSY active as well)
        su_min = 30.0; su_max = 60.0; ho_min = 60.0; ho_max = 120.0;
        gl_pct = 50;
        pop_pct = 60;
        for (k = 0; k < 300; k = k + 1) begin
            sel = ($random(seed) & 32'h7fffffff) % 8;
            if (sel < 6)       send_record(48);
            else if (sel < 7)  send_record(1 + (($random(seed) & 32'h7fffffff) % 47)); // aborted
            else               send_garbage(1 + (($random(seed) & 32'h7fffffff) % 7));
        end
        send_record(48);
        gl_pct = 0;
        pop_pct = 100;
        wait_drained; check_counts("I");
        $display("INFO phase I: glitches injected: crosstalk %0d, ringback %0d, mid-hold %0d (widths %0.1f-%0.1f ns)",
                 n_gl_xtalk, n_gl_ring, n_gl_mid, gl_wmin, gl_wmax);
        if (n_gl_xtalk < 100 || n_gl_ring < 100 || n_gl_mid < 100) err("phase I injected too few glitches");

        $display("INFO totals: %0d transfers, %0d records sent, %0d words pushed, %0d popped, clock phase %0.3f ns",
                 n_xfers, n_records, n_pushed, n_popped, phase0);
        if (errors == 0) $display("PASS tb_par_rx (seed %0d)", seed0);
        else             $display("FAIL tb_par_rx (%0d errors)", errors);
        $finish;
    end

    initial begin
        #(200_000_000.0);
        $display("FAIL tb_par_rx: global timeout");
        $finish;
    end
endmodule
