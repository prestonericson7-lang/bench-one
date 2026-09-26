/* tb_zaccel_gemv.v -- drives zaccel_gemv with gen_vectors.py's stimulus under random input gaps and
 * random output stalls, and checks every output beat bit for bit.
 *   plusargs: +vgap=<0..99>  percent of cycles the source withholds tvalid
 *             +stall=<0..99> percent of cycles the sink drops tready
 *             +seed=<n> */
`timescale 1ns/1ps
`default_nettype none
`include "counts.vh"

module tb;
    reg clk = 0, rstn = 0;
    always #5 clk = ~clk;

    reg  [63:0] in_mem   [0:`NIN-1];
    reg  [63:0] exp_mem  [0:`NOUT-1];
    reg  [63:0] mask_mem [0:`NOUT-1];
    reg         last_mem [0:`NOUT-1];
    initial begin
        $readmemh("in.hex", in_mem);
        $readmemh("exp.hex", exp_mem);
        $readmemh("expmask.hex", mask_mem);
        $readmemh("explast.hex", last_mem);
    end

    integer vgap = 0, stall = 0, seed = 1, seed2 = 99;
    initial begin
        if (!$value$plusargs("vgap=%d", vgap)) vgap = 0;
        if (!$value$plusargs("stall=%d", stall)) stall = 0;
        if (!$value$plusargs("seed=%d", seed)) seed = 1;
        seed2 = seed * 7 + 13;
    end

    reg  [63:0] s_tdata;
    reg         s_tvalid = 0;
    wire        s_tready;
    wire [63:0] m_tdata;
    wire [7:0]  m_tkeep;
    wire        m_tvalid, m_tlast;
    reg         m_tready = 0;

    zaccel_gemv dut (
        .aclk(clk), .aresetn(rstn),
        .s_axis_tdata(s_tdata), .s_axis_tvalid(s_tvalid), .s_axis_tready(s_tready), .s_axis_tlast(1'b0),
        .m_axis_tdata(m_tdata), .m_axis_tkeep(m_tkeep), .m_axis_tvalid(m_tvalid),
        .m_axis_tready(m_tready), .m_axis_tlast(m_tlast));

    integer ip = 0, op = 0, errors = 0, cyc = 0, r;

    /* source: AXI-Stream rules -- once valid, data holds until accepted */
    always @(posedge clk) begin
        if (rstn) begin
            if (s_tvalid && s_tready) begin
                ip = ip + 1;
                s_tvalid <= 1'b0;
            end
            if ((!s_tvalid || s_tready) && ip < `NIN) begin
                r = $urandom(seed) % 100; seed = seed + 1;
                if (r >= vgap) begin
                    s_tdata  <= in_mem[ip];
                    s_tvalid <= 1'b1;
                end else if (s_tvalid && s_tready) s_tvalid <= 1'b0;
            end
        end
    end

    /* sink */
    always @(posedge clk) begin
        if (rstn) begin
            r = $urandom(seed2) % 100; seed2 = seed2 + 3;
            m_tready <= (r >= stall);
            if (m_tvalid && m_tready) begin
                if (op >= `NOUT) begin
                    $display("FAIL extra output beat %016x", m_tdata); errors = errors + 1;
                end else begin
                    if (((m_tdata ^ exp_mem[op]) & mask_mem[op]) != 64'd0 || m_tlast !== last_mem[op] || m_tkeep !== 8'hFF) begin
                        if (errors < 12)
                            $display("FAIL beat %0d: got %016x last %b, want %016x last %b (mask %016x)",
                                     op, m_tdata, m_tlast, exp_mem[op], last_mem[op], mask_mem[op]);
                        errors = errors + 1;
                    end
                    if (last_mem[op] && m_tdata[31:0] == 32'd0) begin
                        $display("FAIL beat %0d: trailer cycle count is zero", op); errors = errors + 1;
                    end
                end
                op = op + 1;
            end
        end
    end

    initial begin
        repeat (5) @(posedge clk);
        rstn = 1;
        while (op < `NOUT && cyc < 4000000) begin @(posedge clk); cyc = cyc + 1; end
        repeat (50) @(posedge clk);
        if (op != `NOUT) begin $display("FAIL timeout: %0d of %0d output beats, %0d of %0d input beats", op, `NOUT, ip, `NIN); errors = errors + 1; end
        if (ip != `NIN)  begin $display("FAIL only %0d of %0d input beats consumed", ip, `NIN); errors = errors + 1; end
        if (errors == 0) $display("ALL PASS  %0d jobs, %0d in / %0d out beats, %0d cycles (vgap %0d%%, stall %0d%%)",
                                  `NJOBS, `NIN, `NOUT, cyc, vgap, stall);
        else             $display("FAIL  %0d errors", errors);
        $finish;
    end
endmodule
`default_nettype wire
