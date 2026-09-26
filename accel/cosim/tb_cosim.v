/* tb_cosim.v -- the matrix engine behind the REAL Xilinx AXI DMA IP (configured exactly as in
 * build_system.tcl), with the engine reset wired as on the board (gemv_reset.v), driven by the exact
 * register sequence zaccel-server's pl_gemv() uses. Memory is a 1 MB AXI slave model standing in for
 * the Zynq's DDR3 through HP3, with random ready/valid gaps on every channel.
 * Proves what the server's software model could not: PG021 behaviour as the IP really implements it
 * (reset/run, Idle after each LENGTH write, S2MM_LENGTH readback), chunked weight transfers, and that
 * a DMA soft reset clears an aborted job in the engine. */
`timescale 1ns/1ps
`default_nettype none
`include "counts.vh"

module tb;
    reg clk = 0; always #5 clk = ~clk;            /* FCLK0 100 MHz */
    reg sys_rstn = 0;
    integer seed = 7;

    /* ---------------- memory model (HP3 stand-in) ---------------- */
    reg [63:0] mem [0:`MEMW-1];
    reg [31:0] jobf [0:`NJOBS*8-1];
    reg [63:0] exp_mem [0:`NEXP-1];
    reg [63:0] mask_mem [0:`NEXP-1];
    initial begin
        $readmemh(`MEM_HEX, mem);
        $readmemh(`JOBS_HEX, jobf);
        $readmemh(`EXP_HEX, exp_mem);
        $readmemh(`EXPMASK_HEX, mask_mem);
    end

    /* ---------------- DMA <-> engine ---------------- */
    reg  [9:0]  l_awaddr, l_araddr;
    reg         l_awvalid = 0, l_wvalid = 0, l_bready = 0, l_arvalid = 0, l_rready = 0;
    reg  [31:0] l_wdata;
    wire        l_awready, l_wready, l_bvalid, l_arready, l_rvalid;
    wire [1:0]  l_bresp, l_rresp;
    wire [31:0] l_rdata;

    wire [31:0] mm_araddr; wire [7:0] mm_arlen; wire [2:0] mm_arsize, mm_arprot; wire [1:0] mm_arburst;
    wire [3:0]  mm_arcache; wire mm_arvalid; reg mm_arready;
    reg  [63:0] mm_rdata; reg mm_rlast, mm_rvalid; wire mm_rready;
    wire [31:0] s2_awaddr; wire [7:0] s2_awlen; wire [2:0] s2_awsize, s2_awprot; wire [1:0] s2_awburst;
    wire [3:0]  s2_awcache; wire s2_awvalid; reg s2_awready;
    wire [63:0] s2_wdata; wire [7:0] s2_wstrb; wire s2_wlast, s2_wvalid; reg s2_wready;
    reg  s2_bvalid; wire s2_bready;

    wire [63:0] mm2s_tdata;  wire [7:0] mm2s_tkeep;  wire mm2s_tvalid, mm2s_tready, mm2s_tlast;
    wire [63:0] s2mm_tdata;  wire [7:0] s2mm_tkeep;  wire s2mm_tvalid, s2mm_tready, s2mm_tlast;
    wire mm2s_rst_n, s2mm_rst_n, eng_rstn, mm2s_irq, s2mm_irq;
    wire [31:0] tstvec;

    dma0 dma (
        .s_axi_lite_aclk(clk), .m_axi_mm2s_aclk(clk), .m_axi_s2mm_aclk(clk), .axi_resetn(sys_rstn),
        .s_axi_lite_awvalid(l_awvalid), .s_axi_lite_awready(l_awready), .s_axi_lite_awaddr(l_awaddr),
        .s_axi_lite_wvalid(l_wvalid), .s_axi_lite_wready(l_wready), .s_axi_lite_wdata(l_wdata),
        .s_axi_lite_bresp(l_bresp), .s_axi_lite_bvalid(l_bvalid), .s_axi_lite_bready(l_bready),
        .s_axi_lite_arvalid(l_arvalid), .s_axi_lite_arready(l_arready), .s_axi_lite_araddr(l_araddr),
        .s_axi_lite_rvalid(l_rvalid), .s_axi_lite_rready(l_rready), .s_axi_lite_rdata(l_rdata), .s_axi_lite_rresp(l_rresp),
        .m_axi_mm2s_araddr(mm_araddr), .m_axi_mm2s_arlen(mm_arlen), .m_axi_mm2s_arsize(mm_arsize),
        .m_axi_mm2s_arburst(mm_arburst), .m_axi_mm2s_arprot(mm_arprot), .m_axi_mm2s_arcache(mm_arcache),
        .m_axi_mm2s_arvalid(mm_arvalid), .m_axi_mm2s_arready(mm_arready),
        .m_axi_mm2s_rdata(mm_rdata), .m_axi_mm2s_rresp(2'b00), .m_axi_mm2s_rlast(mm_rlast),
        .m_axi_mm2s_rvalid(mm_rvalid), .m_axi_mm2s_rready(mm_rready),
        .mm2s_prmry_reset_out_n(mm2s_rst_n),
        .m_axis_mm2s_tdata(mm2s_tdata), .m_axis_mm2s_tkeep(mm2s_tkeep), .m_axis_mm2s_tvalid(mm2s_tvalid),
        .m_axis_mm2s_tready(mm2s_tready), .m_axis_mm2s_tlast(mm2s_tlast),
        .m_axi_s2mm_awaddr(s2_awaddr), .m_axi_s2mm_awlen(s2_awlen), .m_axi_s2mm_awsize(s2_awsize),
        .m_axi_s2mm_awburst(s2_awburst), .m_axi_s2mm_awprot(s2_awprot), .m_axi_s2mm_awcache(s2_awcache),
        .m_axi_s2mm_awvalid(s2_awvalid), .m_axi_s2mm_awready(s2_awready),
        .m_axi_s2mm_wdata(s2_wdata), .m_axi_s2mm_wstrb(s2_wstrb), .m_axi_s2mm_wlast(s2_wlast),
        .m_axi_s2mm_wvalid(s2_wvalid), .m_axi_s2mm_wready(s2_wready),
        .m_axi_s2mm_bresp(2'b00), .m_axi_s2mm_bvalid(s2_bvalid), .m_axi_s2mm_bready(s2_bready),
        .s2mm_prmry_reset_out_n(s2mm_rst_n),
        .s_axis_s2mm_tdata(s2mm_tdata), .s_axis_s2mm_tkeep(s2mm_tkeep), .s_axis_s2mm_tvalid(s2mm_tvalid),
        .s_axis_s2mm_tready(s2mm_tready), .s_axis_s2mm_tlast(s2mm_tlast),
        .mm2s_introut(mm2s_irq), .s2mm_introut(s2mm_irq), .axi_dma_tstvec(tstvec));

    gemv_reset grst (.aclk(clk), .peripheral_aresetn(sys_rstn), .mm2s_prmry_reset_out_n(mm2s_rst_n),
                     .s2mm_prmry_reset_out_n(s2mm_rst_n), .aresetn(eng_rstn));

    zaccel_gemv eng (.aclk(clk), .aresetn(eng_rstn),
        .s_axis_tdata(mm2s_tdata), .s_axis_tvalid(mm2s_tvalid), .s_axis_tready(mm2s_tready), .s_axis_tlast(mm2s_tlast),
        .m_axis_tdata(s2mm_tdata), .m_axis_tkeep(s2mm_tkeep), .m_axis_tvalid(s2mm_tvalid),
        .m_axis_tready(s2mm_tready), .m_axis_tlast(s2mm_tlast));

    /* ---------------- AXI slave: MM2S reads (queue of bursts, random gaps) ---------------- */
    reg [31:0] arq_a [0:15]; reg [7:0] arq_l [0:15]; reg [4:0] arq_n; reg [3:0] arq_w, arq_r;
    reg        rd_act; reg [31:0] rd_a; reg [7:0] rd_l; reg [8:0] rd_i;   /* rd_i: next beat to present */
    reg        pop;
    integer r;
    always @(posedge clk) begin
        if (!sys_rstn) begin
            mm_arready <= 0; mm_rvalid <= 0; mm_rlast <= 0; rd_act <= 0; arq_n <= 0; arq_w <= 0; arq_r <= 0;
        end else begin
            if (mm_arvalid && mm_arready) begin arq_a[arq_w] <= mm_araddr; arq_l[arq_w] <= mm_arlen; arq_w <= arq_w + 1; end
            r = $random(seed); mm_arready <= (arq_n < 12) && (r[3:0] > 3);
            if (mm_rvalid && mm_rready) begin mm_rvalid <= 0; if (mm_rlast) rd_act <= 0; end
            pop = 0;
            if (!rd_act && arq_n != 0 && !mm_rvalid) begin
                rd_act <= 1; rd_a <= arq_a[arq_r]; rd_l <= arq_l[arq_r]; rd_i <= 0; arq_r <= arq_r + 1; pop = 1;
            end
            arq_n <= arq_n + ((mm_arvalid && mm_arready) ? 1 : 0) - (pop ? 1 : 0);
            if (rd_act && (!mm_rvalid || mm_rready) && !(mm_rvalid && mm_rready && mm_rlast) && rd_i <= rd_l) begin
                r = $random(seed);
                if (r[2:0] > 1) begin
                    mm_rvalid <= 1; mm_rdata <= mem[((rd_a >> 3) + rd_i) % `MEMW]; mm_rlast <= (rd_i == rd_l);
                    rd_i <= rd_i + 1;
                end
            end
        end
    end

    /* ---------------- AXI slave: S2MM writes ---------------- */
    reg [31:0] awq_a [0:15]; reg [3:0] awq_w, awq_r; reg [4:0] awq_n;
    reg        wr_act; reg [31:0] wr_a; reg [8:0] wr_i; reg [4:0] bq;
    reg        wpop, wdone, bdone;
    integer k;
    always @(posedge clk) begin
        if (!sys_rstn) begin
            s2_awready <= 0; s2_wready <= 0; s2_bvalid <= 0; wr_act <= 0; awq_n <= 0; awq_w <= 0; awq_r <= 0; bq <= 0;
        end else begin
            if (s2_awvalid && s2_awready) begin awq_a[awq_w] <= s2_awaddr; awq_w <= awq_w + 1; end
            r = $random(seed); s2_awready <= (awq_n < 12) && (r[3:0] > 4);
            wdone = s2_wvalid && s2_wready && s2_wlast;
            if (s2_wvalid && s2_wready) begin
                for (k = 0; k < 8; k = k + 1)
                    if (s2_wstrb[k]) mem[((wr_a >> 3) + wr_i) % `MEMW][k*8 +: 8] <= s2_wdata[k*8 +: 8];
                wr_i <= wr_i + 1;
                if (s2_wlast) wr_act <= 0;
            end
            wpop = 0;
            if (!wr_act && awq_n != 0) begin
                wr_act <= 1; wr_a <= awq_a[awq_r]; wr_i <= 0; awq_r <= awq_r + 1; wpop = 1;
            end
            awq_n <= awq_n + ((s2_awvalid && s2_awready) ? 1 : 0) - (wpop ? 1 : 0);
            r = $random(seed);
            s2_wready <= ((wr_act && !wdone) || wpop) && (r[2:0] > 1);
            bdone = s2_bvalid && s2_bready;
            bq <= bq + (wdone ? 1 : 0) - (bdone ? 1 : 0);
            s2_bvalid <= (bq + (wdone ? 1 : 0) - (bdone ? 1 : 0)) != 0;
        end
    end

    /* ---------------- AXI-Lite master (the CPU) ---------------- */
    task lwr(input [9:0] a, input [31:0] d);
        reg aw_ok, w_ok;
        begin
            @(posedge clk); l_awaddr <= a; l_wdata <= d; l_awvalid <= 1; l_wvalid <= 1; l_bready <= 1;
            aw_ok = 0; w_ok = 0;
            while (!(aw_ok && w_ok)) begin
                @(posedge clk);
                if (l_awvalid && l_awready) begin aw_ok = 1; l_awvalid <= 0; end
                if (l_wvalid && l_wready) begin w_ok = 1; l_wvalid <= 0; end
            end
            while (!l_bvalid) @(posedge clk);
            @(posedge clk); l_bready <= 0;
        end
    endtask
    task lrd(input [9:0] a, output [31:0] d);
        begin
            @(posedge clk); l_araddr <= a; l_arvalid <= 1; l_rready <= 1;
            @(posedge clk); while (!l_arready) @(posedge clk);
            l_arvalid <= 0;
            while (!l_rvalid) @(posedge clk);
            d = l_rdata;
            @(posedge clk); l_rready <= 0;
        end
    endtask

    integer errors = 0, t;
    reg [31:0] v;
    task wait_bits(input [9:0] a, input [31:0] m, input [31:0] want, input [8*24-1:0] what);
        begin
            t = 0; lrd(a, v);
            while ((v & m) != want && t < 20000) begin lrd(a, v); t = t + 1; end
            if ((v & m) != want) begin $display("FAIL timeout waiting for %0s (reg %03x = %08x)", what, a, v); errors = errors + 1; end
        end
    endtask
    /* the server's dma_wait_idle: error bits fatal, Idle = done, Halted = fatal */
    task wait_idle(input [9:0] a, input [8*24-1:0] what);
        begin
            t = 0; lrd(a, v);
            while (!(v & 32'h2) && !(v & 32'h4770) && !(v & 32'h1) && t < 200000) begin lrd(a, v); t = t + 1; end
            if (v & 32'h4770)      begin $display("FAIL %0s: DMA error, DMASR=%08x", what, v); errors = errors + 1; end
            else if (!(v & 32'h2)) begin $display("FAIL %0s: not idle, DMASR=%08x (polls %0d)", what, v, t); errors = errors + 1; end
        end
    endtask

    integer j, i, eo, nw, chunk, left, a, n, abort, jerr;
    reg [31:0] in_a, in_b, w_a, w_b, o_a, o_b, got;
    initial begin
        repeat (20) @(posedge clk);
        sys_rstn = 1;
        repeat (20) @(posedge clk);
        for (j = 0; j < `NJOBS; j = j + 1) begin
            in_a = jobf[j*8]; in_b = jobf[j*8+1]; w_a = jobf[j*8+2]; w_b = jobf[j*8+3];
            o_a = jobf[j*8+4]; o_b = jobf[j*8+5]; chunk = jobf[j*8+6]; abort = jobf[j*8+7] & 1; eo = jobf[j*8+7] >> 8;
            jerr = errors;
            /* dma_reset(): MM2S then S2MM soft reset, wait for the bit to clear */
            lwr(10'h00, 32'h4); wait_bits(10'h00, 32'h4, 0, "MM2S reset");
            lwr(10'h30, 32'h4); wait_bits(10'h30, 32'h4, 0, "S2MM reset");
            /* dma_run() */
            lwr(10'h00, 32'h1); lwr(10'h30, 32'h1);
            wait_bits(10'h04, 32'h1, 0, "MM2S run"); wait_bits(10'h34, 32'h1, 0, "S2MM run");
            /* results armed first, then header + activations, then the weights in chunks */
            lwr(10'h48, o_a); lwr(10'h58, o_b);
            lwr(10'h18, in_a); lwr(10'h28, in_b);
            wait_idle(10'h04, "MM2S header+acts");
            left = w_b; a = w_a;
            while (left > 0) begin
                n = (left > chunk) ? chunk : left;
                lwr(10'h18, a); lwr(10'h28, n);
                wait_idle(10'h04, "MM2S weights");
                a = a + n; left = left - n;
                if (abort) left = 0;                 /* cut the job short */
            end
            if (abort) begin
                $display("job %0d: aborted after %0d of %0d weight bytes (next job starts with the server's soft reset)", j, chunk, w_b);
            end else begin
                wait_idle(10'h34, "S2MM results");
                lrd(10'h58, got);
                if ((got & 32'h3FFFFFF) != o_b) begin $display("FAIL job %0d: S2MM_LENGTH %0d, want %0d", j, got & 32'h3FFFFFF, o_b); errors = errors + 1; end
                nw = o_b / 8;
                for (i = 0; i < nw; i = i + 1)
                    if (((mem[(o_a >> 3) + i] ^ exp_mem[eo + i]) & mask_mem[eo + i]) != 64'd0) begin
                        if (errors - jerr < 6) $display("FAIL job %0d word %0d: got %016x want %016x", j, i, mem[(o_a >> 3) + i], exp_mem[eo + i]);
                        errors = errors + 1;
                    end
                if (mem[(o_a >> 3) + nw - 1][31:0] == 0) begin $display("FAIL job %0d: trailer cycles 0", j); errors = errors + 1; end
                $display("job %0d: %0s  (%0d result bytes, S2MM_LENGTH %0d, engine cycles %0d)", j,
                         (errors == jerr) ? "exact" : "WRONG", o_b, got & 32'h3FFFFFF, mem[(o_a >> 3) + nw - 1][31:0]);
            end
        end
        if (errors == 0) $display("COSIM PASS: real AXI DMA + engine + reset wiring, server register sequence, %0d jobs", `NJOBS);
        else             $display("COSIM FAIL: %0d errors", errors);
        $finish;
    end
    initial begin #50000000; $display("COSIM FAIL: global timeout"); $finish; end
endmodule
`default_nettype wire
