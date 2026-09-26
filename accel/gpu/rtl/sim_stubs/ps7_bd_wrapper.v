`ifndef VERILATOR
`timescale 1ns / 1ps   // delays in this stub are in ns (iverilog only)
`endif
// ps7_bd_wrapper.v -- SIMULATION/LINT-ONLY stand-in for the Vivado-generated block-design wrapper
// of fpga/build.tcl (block design 'ps7_bd': processing_system7 with M_AXI_GP0 and S_AXI_HP0/1/2).
// Never synthesise this file: Vivado builds the real wrapper with make_wrapper.
//
// PORT NAMES -- exactly the ports of the wrapper Vivado 2026.1 generates for the block design of
// fpga/build.tcl (177 ports, same names, directions, widths and order as the real run recorded
// in fpga/probe/ps7_bd_wrapper_ports.txt). build.tcl compares both port lists after make_wrapper
// and stops with a diff if they ever differ. Vivado's naming rules:
//   * external interface port <P> exposes one wrapper port per physical signal, named
//     <P>_<logical name in lower case> (e.g. M_AXI_GP0_araddr, S_AXI_HP1_wstrb, DDR_dqs_p);
//   * plain external ports keep their name (M_AXI_GP0_ACLK, S_AXI_HP0_ACLK, ...);
//   * the AXI signal set is the one of the PS7 pin the port is cloned from
//     (make_bd_intf_pins_external): AXI3, lock[1:0], qos[3:0], cache, prot, id (GP0 12 bits,
//     HP 6 bits), no region/user signals.
// Interface ports created by build.tcl (names identical to the PS7 pins):
//   DDR, FIXED_IO  (apply_bd_automation, standard names)
//   M_AXI_GP0      <- processing_system7_0/M_AXI_GP0   (Master: the PL is the slave)
//   S_AXI_HP0/1/2  <- processing_system7_0/S_AXI_HP0/1/2 (Slave: the PL is the master), 64-bit
//   M_AXI_GP0_ACLK, S_AXI_HP0_ACLK, S_AXI_HP1_ACLK, S_AXI_HP2_ACLK: clock ports, 148.75 MHz
//
// Behaviour (iverilog only; under Verilator every output is a constant):
//  * GP0: a bus-functional AXI3 master driven by the tasks gp0_write(addr, data) and
//    gp0_read(addr, data) (call hierarchically, e.g. tb.dut.u_ps7.gp0_write(32'h43C00008, 4)).
//    They check BRESP/RRESP = OKAY, BID/RID echo and RLAST.
//  * HP0/1/2: AXI3 slaves on one shared DDR model  ddr[addr[MEM_AW+2:3]]  (64-bit words;
//    MEM_AW = 21 -> a 16 MB window that aliases, covering 0x1E000000..0x1EFFFFFF), zero-filled.
//    Random READY/VALID stalls (STALLS = 1), read latency RD_LATENCY cycles.
//    Protocol checks per SPEC section 9 (size 3, INCR, len <= 15, id 0, cache 0011, prot/lock/qos
//    0, WSTRB FF, WID 0, WLAST on the last beat, no 4 KB crossing, 8-byte aligned, VALID/payload
//    stable until READY, write channel used only on HP1 and read channel only on HP0/HP2).
//    Violations are printed and counted in stub_errors.
/* verilator lint_off UNUSED */   // stub: most inputs are intentionally ignored
module ps7_bd_wrapper (
    // ---- PS DDR (pass-through to the PS7 pins) ----
    inout  wire [14:0]  DDR_addr,
    inout  wire [2:0]   DDR_ba,
    inout  wire         DDR_cas_n,
    inout  wire         DDR_ck_n,
    inout  wire         DDR_ck_p,
    inout  wire         DDR_cke,
    inout  wire         DDR_cs_n,
    inout  wire [3:0]   DDR_dm,
    inout  wire [31:0]  DDR_dq,
    inout  wire [3:0]   DDR_dqs_n,
    inout  wire [3:0]   DDR_dqs_p,
    inout  wire         DDR_odt,
    inout  wire         DDR_ras_n,
    inout  wire         DDR_reset_n,
    inout  wire         DDR_we_n,
    // ---- PS fixed IO ----
    inout  wire         FIXED_IO_ddr_vrn,
    inout  wire         FIXED_IO_ddr_vrp,
    inout  wire [53:0]  FIXED_IO_mio,
    inout  wire         FIXED_IO_ps_clk,
    inout  wire         FIXED_IO_ps_porb,
    inout  wire         FIXED_IO_ps_srstb,
    // ---- M_AXI_GP0 (PS master, 32-bit AXI3, 12-bit IDs; the PL is the slave) ----
    input  wire         M_AXI_GP0_ACLK,
    output wire [31:0]  M_AXI_GP0_araddr,
    output wire [1:0]   M_AXI_GP0_arburst,
    output wire [3:0]   M_AXI_GP0_arcache,
    output wire [11:0]  M_AXI_GP0_arid,
    output wire [3:0]   M_AXI_GP0_arlen,
    output wire [1:0]   M_AXI_GP0_arlock,
    output wire [2:0]   M_AXI_GP0_arprot,
    output wire [3:0]   M_AXI_GP0_arqos,
    input  wire         M_AXI_GP0_arready,
    output wire [2:0]   M_AXI_GP0_arsize,
    output wire         M_AXI_GP0_arvalid,
    output wire [31:0]  M_AXI_GP0_awaddr,
    output wire [1:0]   M_AXI_GP0_awburst,
    output wire [3:0]   M_AXI_GP0_awcache,
    output wire [11:0]  M_AXI_GP0_awid,
    output wire [3:0]   M_AXI_GP0_awlen,
    output wire [1:0]   M_AXI_GP0_awlock,
    output wire [2:0]   M_AXI_GP0_awprot,
    output wire [3:0]   M_AXI_GP0_awqos,
    input  wire         M_AXI_GP0_awready,
    output wire [2:0]   M_AXI_GP0_awsize,
    output wire         M_AXI_GP0_awvalid,
    input  wire [11:0]  M_AXI_GP0_bid,
    output wire         M_AXI_GP0_bready,
    input  wire [1:0]   M_AXI_GP0_bresp,
    input  wire         M_AXI_GP0_bvalid,
    input  wire [31:0]  M_AXI_GP0_rdata,
    input  wire [11:0]  M_AXI_GP0_rid,
    input  wire         M_AXI_GP0_rlast,
    output wire         M_AXI_GP0_rready,
    input  wire [1:0]   M_AXI_GP0_rresp,
    input  wire         M_AXI_GP0_rvalid,
    output wire [31:0]  M_AXI_GP0_wdata,
    output wire [11:0]  M_AXI_GP0_wid,
    output wire         M_AXI_GP0_wlast,
    input  wire         M_AXI_GP0_wready,
    output wire [3:0]   M_AXI_GP0_wstrb,
    output wire         M_AXI_GP0_wvalid,
    // ---- S_AXI_HP0 (64-bit AXI3 slave, 6-bit IDs; scanout reads) ----
    input  wire         S_AXI_HP0_ACLK,
    input  wire [31:0]  S_AXI_HP0_araddr,
    input  wire [1:0]   S_AXI_HP0_arburst,
    input  wire [3:0]   S_AXI_HP0_arcache,
    input  wire [5:0]   S_AXI_HP0_arid,
    input  wire [3:0]   S_AXI_HP0_arlen,
    input  wire [1:0]   S_AXI_HP0_arlock,
    input  wire [2:0]   S_AXI_HP0_arprot,
    input  wire [3:0]   S_AXI_HP0_arqos,
    output wire         S_AXI_HP0_arready,
    input  wire [2:0]   S_AXI_HP0_arsize,
    input  wire         S_AXI_HP0_arvalid,
    input  wire [31:0]  S_AXI_HP0_awaddr,
    input  wire [1:0]   S_AXI_HP0_awburst,
    input  wire [3:0]   S_AXI_HP0_awcache,
    input  wire [5:0]   S_AXI_HP0_awid,
    input  wire [3:0]   S_AXI_HP0_awlen,
    input  wire [1:0]   S_AXI_HP0_awlock,
    input  wire [2:0]   S_AXI_HP0_awprot,
    input  wire [3:0]   S_AXI_HP0_awqos,
    output wire         S_AXI_HP0_awready,
    input  wire [2:0]   S_AXI_HP0_awsize,
    input  wire         S_AXI_HP0_awvalid,
    output wire [5:0]   S_AXI_HP0_bid,
    input  wire         S_AXI_HP0_bready,
    output wire [1:0]   S_AXI_HP0_bresp,
    output wire         S_AXI_HP0_bvalid,
    output wire [63:0]  S_AXI_HP0_rdata,
    output wire [5:0]   S_AXI_HP0_rid,
    output wire         S_AXI_HP0_rlast,
    input  wire         S_AXI_HP0_rready,
    output wire [1:0]   S_AXI_HP0_rresp,
    output wire         S_AXI_HP0_rvalid,
    input  wire [63:0]  S_AXI_HP0_wdata,
    input  wire [5:0]   S_AXI_HP0_wid,
    input  wire         S_AXI_HP0_wlast,
    output wire         S_AXI_HP0_wready,
    input  wire [7:0]   S_AXI_HP0_wstrb,
    input  wire         S_AXI_HP0_wvalid,
    // ---- S_AXI_HP1 (64-bit AXI3 slave; strip writes) ----
    input  wire         S_AXI_HP1_ACLK,
    input  wire [31:0]  S_AXI_HP1_araddr,
    input  wire [1:0]   S_AXI_HP1_arburst,
    input  wire [3:0]   S_AXI_HP1_arcache,
    input  wire [5:0]   S_AXI_HP1_arid,
    input  wire [3:0]   S_AXI_HP1_arlen,
    input  wire [1:0]   S_AXI_HP1_arlock,
    input  wire [2:0]   S_AXI_HP1_arprot,
    input  wire [3:0]   S_AXI_HP1_arqos,
    output wire         S_AXI_HP1_arready,
    input  wire [2:0]   S_AXI_HP1_arsize,
    input  wire         S_AXI_HP1_arvalid,
    input  wire [31:0]  S_AXI_HP1_awaddr,
    input  wire [1:0]   S_AXI_HP1_awburst,
    input  wire [3:0]   S_AXI_HP1_awcache,
    input  wire [5:0]   S_AXI_HP1_awid,
    input  wire [3:0]   S_AXI_HP1_awlen,
    input  wire [1:0]   S_AXI_HP1_awlock,
    input  wire [2:0]   S_AXI_HP1_awprot,
    input  wire [3:0]   S_AXI_HP1_awqos,
    output wire         S_AXI_HP1_awready,
    input  wire [2:0]   S_AXI_HP1_awsize,
    input  wire         S_AXI_HP1_awvalid,
    output wire [5:0]   S_AXI_HP1_bid,
    input  wire         S_AXI_HP1_bready,
    output wire [1:0]   S_AXI_HP1_bresp,
    output wire         S_AXI_HP1_bvalid,
    output wire [63:0]  S_AXI_HP1_rdata,
    output wire [5:0]   S_AXI_HP1_rid,
    output wire         S_AXI_HP1_rlast,
    input  wire         S_AXI_HP1_rready,
    output wire [1:0]   S_AXI_HP1_rresp,
    output wire         S_AXI_HP1_rvalid,
    input  wire [63:0]  S_AXI_HP1_wdata,
    input  wire [5:0]   S_AXI_HP1_wid,
    input  wire         S_AXI_HP1_wlast,
    output wire         S_AXI_HP1_wready,
    input  wire [7:0]   S_AXI_HP1_wstrb,
    input  wire         S_AXI_HP1_wvalid,
    // ---- S_AXI_HP2 (64-bit AXI3 slave; sprite reads) ----
    input  wire         S_AXI_HP2_ACLK,
    input  wire [31:0]  S_AXI_HP2_araddr,
    input  wire [1:0]   S_AXI_HP2_arburst,
    input  wire [3:0]   S_AXI_HP2_arcache,
    input  wire [5:0]   S_AXI_HP2_arid,
    input  wire [3:0]   S_AXI_HP2_arlen,
    input  wire [1:0]   S_AXI_HP2_arlock,
    input  wire [2:0]   S_AXI_HP2_arprot,
    input  wire [3:0]   S_AXI_HP2_arqos,
    output wire         S_AXI_HP2_arready,
    input  wire [2:0]   S_AXI_HP2_arsize,
    input  wire         S_AXI_HP2_arvalid,
    input  wire [31:0]  S_AXI_HP2_awaddr,
    input  wire [1:0]   S_AXI_HP2_awburst,
    input  wire [3:0]   S_AXI_HP2_awcache,
    input  wire [5:0]   S_AXI_HP2_awid,
    input  wire [3:0]   S_AXI_HP2_awlen,
    input  wire [1:0]   S_AXI_HP2_awlock,
    input  wire [2:0]   S_AXI_HP2_awprot,
    input  wire [3:0]   S_AXI_HP2_awqos,
    output wire         S_AXI_HP2_awready,
    input  wire [2:0]   S_AXI_HP2_awsize,
    input  wire         S_AXI_HP2_awvalid,
    output wire [5:0]   S_AXI_HP2_bid,
    input  wire         S_AXI_HP2_bready,
    output wire [1:0]   S_AXI_HP2_bresp,
    output wire         S_AXI_HP2_bvalid,
    output wire [63:0]  S_AXI_HP2_rdata,
    output wire [5:0]   S_AXI_HP2_rid,
    output wire         S_AXI_HP2_rlast,
    input  wire         S_AXI_HP2_rready,
    output wire [1:0]   S_AXI_HP2_rresp,
    output wire         S_AXI_HP2_rvalid,
    input  wire [63:0]  S_AXI_HP2_wdata,
    input  wire [5:0]   S_AXI_HP2_wid,
    input  wire         S_AXI_HP2_wlast,
    output wire         S_AXI_HP2_wready,
    input  wire [7:0]   S_AXI_HP2_wstrb,
    input  wire         S_AXI_HP2_wvalid
);
`ifdef VERILATOR

    assign M_AXI_GP0_araddr = 32'd0;  assign M_AXI_GP0_arburst = 2'b01; assign M_AXI_GP0_arcache = 4'd0;
    assign M_AXI_GP0_arid   = 12'd0;  assign M_AXI_GP0_arlen   = 4'd0;  assign M_AXI_GP0_arlock  = 2'd0;
    assign M_AXI_GP0_arprot = 3'd0;   assign M_AXI_GP0_arqos   = 4'd0;  assign M_AXI_GP0_arsize  = 3'd2;
    assign M_AXI_GP0_arvalid = 1'b0;
    assign M_AXI_GP0_awaddr = 32'd0;  assign M_AXI_GP0_awburst = 2'b01; assign M_AXI_GP0_awcache = 4'd0;
    assign M_AXI_GP0_awid   = 12'd0;  assign M_AXI_GP0_awlen   = 4'd0;  assign M_AXI_GP0_awlock  = 2'd0;
    assign M_AXI_GP0_awprot = 3'd0;   assign M_AXI_GP0_awqos   = 4'd0;  assign M_AXI_GP0_awsize  = 3'd2;
    assign M_AXI_GP0_awvalid = 1'b0;
    assign M_AXI_GP0_bready = 1'b1;   assign M_AXI_GP0_rready  = 1'b1;
    assign M_AXI_GP0_wdata  = 32'd0;  assign M_AXI_GP0_wid     = 12'd0; assign M_AXI_GP0_wlast   = 1'b0;
    assign M_AXI_GP0_wstrb  = 4'd0;   assign M_AXI_GP0_wvalid  = 1'b0;
    assign S_AXI_HP0_arready = 1'b0; assign S_AXI_HP0_awready = 1'b0; assign S_AXI_HP0_wready = 1'b0;
    assign S_AXI_HP0_bid = 6'd0; assign S_AXI_HP0_bresp = 2'd0; assign S_AXI_HP0_bvalid = 1'b0;
    assign S_AXI_HP0_rdata = 64'd0; assign S_AXI_HP0_rid = 6'd0; assign S_AXI_HP0_rlast = 1'b0;
    assign S_AXI_HP0_rresp = 2'd0; assign S_AXI_HP0_rvalid = 1'b0;
    assign S_AXI_HP1_arready = 1'b0; assign S_AXI_HP1_awready = 1'b0; assign S_AXI_HP1_wready = 1'b0;
    assign S_AXI_HP1_bid = 6'd0; assign S_AXI_HP1_bresp = 2'd0; assign S_AXI_HP1_bvalid = 1'b0;
    assign S_AXI_HP1_rdata = 64'd0; assign S_AXI_HP1_rid = 6'd0; assign S_AXI_HP1_rlast = 1'b0;
    assign S_AXI_HP1_rresp = 2'd0; assign S_AXI_HP1_rvalid = 1'b0;
    assign S_AXI_HP2_arready = 1'b0; assign S_AXI_HP2_awready = 1'b0; assign S_AXI_HP2_wready = 1'b0;
    assign S_AXI_HP2_bid = 6'd0; assign S_AXI_HP2_bresp = 2'd0; assign S_AXI_HP2_bvalid = 1'b0;
    assign S_AXI_HP2_rdata = 64'd0; assign S_AXI_HP2_rid = 6'd0; assign S_AXI_HP2_rlast = 1'b0;
    assign S_AXI_HP2_rresp = 2'd0; assign S_AXI_HP2_rvalid = 1'b0;

`else
    parameter integer MEM_AW     = 21;   // DDR model: 2**MEM_AW 64-bit words (aliased)
    parameter integer RD_LATENCY = 6;    // AR accepted -> first R beat (cycles)
    parameter integer STALLS     = 1;    // 1 = random READY / VALID gaps on the HP slaves
    parameter integer MAX_MSGS   = 20;   // print at most this many protocol errors

    integer stub_errors = 0;
    integer hp_wr_beats = 0;             // statistics (all ports)
    integer hp_rd_beats = 0;
    integer i;

    reg [63:0] ddr [0:(1<<MEM_AW)-1];
    initial begin
        for (i = 0; i < (1 << MEM_AW); i = i + 1)
            ddr[i] = 64'd0;
    end

    task stub_err;
        input [8*96-1:0] msg;
        begin
            stub_errors = stub_errors + 1;
            if (stub_errors <= MAX_MSGS)
                $display("%t ps7_bd_wrapper stub: PROTOCOL ERROR: %0s", $time, msg);
        end
    endtask

    // ================================ GP0 bus-functional master ================================
    reg [31:0] m_awaddr = 0, m_araddr = 0, m_wdata = 0;
    reg [11:0] m_awid = 0, m_arid = 0;
    reg        m_awvalid = 0, m_wvalid = 0, m_bready = 0, m_arvalid = 0, m_rready = 0;
    reg [11:0] gp0_next_id = 12'h5A0;

    assign M_AXI_GP0_awaddr = m_awaddr; assign M_AXI_GP0_awburst = 2'b01; assign M_AXI_GP0_awcache = 4'b0011;
    assign M_AXI_GP0_awid   = m_awid;   assign M_AXI_GP0_awlen   = 4'd0;  assign M_AXI_GP0_awlock  = 2'b00;
    assign M_AXI_GP0_awprot = 3'b000;   assign M_AXI_GP0_awqos   = 4'd0;  assign M_AXI_GP0_awsize  = 3'd2;
    assign M_AXI_GP0_awvalid = m_awvalid;
    assign M_AXI_GP0_wdata  = m_wdata;  assign M_AXI_GP0_wid = m_awid;    assign M_AXI_GP0_wlast = 1'b1;
    assign M_AXI_GP0_wstrb  = 4'hF;     assign M_AXI_GP0_wvalid = m_wvalid;
    assign M_AXI_GP0_bready = m_bready;
    assign M_AXI_GP0_araddr = m_araddr; assign M_AXI_GP0_arburst = 2'b01; assign M_AXI_GP0_arcache = 4'b0011;
    assign M_AXI_GP0_arid   = m_arid;   assign M_AXI_GP0_arlen   = 4'd0;  assign M_AXI_GP0_arlock  = 2'b00;
    assign M_AXI_GP0_arprot = 3'b000;   assign M_AXI_GP0_arqos   = 4'd0;  assign M_AXI_GP0_arsize  = 3'd2;
    assign M_AXI_GP0_arvalid = m_arvalid;
    assign M_AXI_GP0_rready = m_rready;

    // single-beat write; returns when the B response has been accepted
    task gp0_write;
        input [31:0] addr;
        input [31:0] data;
        reg aw_done, w_done;
        begin
            @(posedge M_AXI_GP0_ACLK);
            m_awaddr  <= addr;  m_awid <= gp0_next_id; m_awvalid <= 1'b1;
            m_wdata   <= data;  m_wvalid <= 1'b1;
            aw_done = 1'b0; w_done = 1'b0;
            while (!(aw_done && w_done)) begin
                @(posedge M_AXI_GP0_ACLK);
                if (m_awvalid && M_AXI_GP0_awready) begin aw_done = 1'b1; m_awvalid <= 1'b0; end
                if (m_wvalid  && M_AXI_GP0_wready)  begin w_done  = 1'b1; m_wvalid  <= 1'b0; end
            end
            m_bready <= 1'b1;
            @(posedge M_AXI_GP0_ACLK);
            while (!M_AXI_GP0_bvalid) @(posedge M_AXI_GP0_ACLK);
            if (M_AXI_GP0_bresp != 2'b00) stub_err("GP0 BRESP != OKAY");
            if (M_AXI_GP0_bid != m_awid)  stub_err("GP0 BID does not echo AWID");
            m_bready <= 1'b0;
            gp0_next_id = gp0_next_id + 12'd1;
        end
    endtask

    // single-beat read
    task gp0_read;
        input  [31:0] addr;
        output [31:0] data;
        begin
            @(posedge M_AXI_GP0_ACLK);
            m_araddr <= addr; m_arid <= gp0_next_id; m_arvalid <= 1'b1;
            @(posedge M_AXI_GP0_ACLK);
            while (!M_AXI_GP0_arready) @(posedge M_AXI_GP0_ACLK);
            m_arvalid <= 1'b0;
            m_rready  <= 1'b1;
            @(posedge M_AXI_GP0_ACLK);
            while (!M_AXI_GP0_rvalid) @(posedge M_AXI_GP0_ACLK);
            data = M_AXI_GP0_rdata;
            if (M_AXI_GP0_rresp != 2'b00) stub_err("GP0 RRESP != OKAY");
            if (M_AXI_GP0_rid != m_arid)  stub_err("GP0 RID does not echo ARID");
            if (!M_AXI_GP0_rlast)         stub_err("GP0 RLAST missing on a single-beat read");
            m_rready <= 1'b0;
            gp0_next_id = gp0_next_id + 12'd1;
        end
    endtask

    // ================================ HP0/1/2 slave models ======================================
    // Port signals gathered into arrays so one generate body serves all three ports.
    wire        s_aclk    [0:2];
    wire [31:0] s_araddr  [0:2];  wire [1:0] s_arburst [0:2];  wire [3:0] s_arcache [0:2];
    wire [5:0]  s_arid    [0:2];  wire [3:0] s_arlen   [0:2];  wire [1:0] s_arlock  [0:2];
    wire [2:0]  s_arprot  [0:2];  wire [3:0] s_arqos   [0:2];  wire [2:0] s_arsize  [0:2];
    wire        s_arvalid [0:2];  wire       s_rready  [0:2];
    wire [31:0] s_awaddr  [0:2];  wire [1:0] s_awburst [0:2];  wire [3:0] s_awcache [0:2];
    wire [5:0]  s_awid    [0:2];  wire [3:0] s_awlen   [0:2];  wire [1:0] s_awlock  [0:2];
    wire [2:0]  s_awprot  [0:2];  wire [3:0] s_awqos   [0:2];  wire [2:0] s_awsize  [0:2];
    wire        s_awvalid [0:2];
    wire [63:0] s_wdata   [0:2];  wire [5:0] s_wid     [0:2];  wire       s_wlast   [0:2];
    wire [7:0]  s_wstrb   [0:2];  wire       s_wvalid  [0:2];  wire       s_bready  [0:2];

    wire        s_arready [0:2];  wire       s_awready [0:2];  wire       s_wready  [0:2];
    wire        s_bvalid  [0:2];  wire [5:0] s_bid     [0:2];
    wire        s_rvalid  [0:2];  wire [63:0] s_rdata  [0:2];  wire       s_rlast   [0:2];
    wire [5:0]  s_rid     [0:2];

    assign s_aclk[0] = S_AXI_HP0_ACLK;       assign s_aclk[1] = S_AXI_HP1_ACLK;       assign s_aclk[2] = S_AXI_HP2_ACLK;
    assign s_araddr[0] = S_AXI_HP0_araddr;   assign s_araddr[1] = S_AXI_HP1_araddr;   assign s_araddr[2] = S_AXI_HP2_araddr;
    assign s_arburst[0] = S_AXI_HP0_arburst; assign s_arburst[1] = S_AXI_HP1_arburst; assign s_arburst[2] = S_AXI_HP2_arburst;
    assign s_arcache[0] = S_AXI_HP0_arcache; assign s_arcache[1] = S_AXI_HP1_arcache; assign s_arcache[2] = S_AXI_HP2_arcache;
    assign s_arid[0] = S_AXI_HP0_arid;       assign s_arid[1] = S_AXI_HP1_arid;       assign s_arid[2] = S_AXI_HP2_arid;
    assign s_arlen[0] = S_AXI_HP0_arlen;     assign s_arlen[1] = S_AXI_HP1_arlen;     assign s_arlen[2] = S_AXI_HP2_arlen;
    assign s_arlock[0] = S_AXI_HP0_arlock;   assign s_arlock[1] = S_AXI_HP1_arlock;   assign s_arlock[2] = S_AXI_HP2_arlock;
    assign s_arprot[0] = S_AXI_HP0_arprot;   assign s_arprot[1] = S_AXI_HP1_arprot;   assign s_arprot[2] = S_AXI_HP2_arprot;
    assign s_arqos[0] = S_AXI_HP0_arqos;     assign s_arqos[1] = S_AXI_HP1_arqos;     assign s_arqos[2] = S_AXI_HP2_arqos;
    assign s_arsize[0] = S_AXI_HP0_arsize;   assign s_arsize[1] = S_AXI_HP1_arsize;   assign s_arsize[2] = S_AXI_HP2_arsize;
    assign s_arvalid[0] = S_AXI_HP0_arvalid; assign s_arvalid[1] = S_AXI_HP1_arvalid; assign s_arvalid[2] = S_AXI_HP2_arvalid;
    assign s_rready[0] = S_AXI_HP0_rready;   assign s_rready[1] = S_AXI_HP1_rready;   assign s_rready[2] = S_AXI_HP2_rready;
    assign s_awaddr[0] = S_AXI_HP0_awaddr;   assign s_awaddr[1] = S_AXI_HP1_awaddr;   assign s_awaddr[2] = S_AXI_HP2_awaddr;
    assign s_awburst[0] = S_AXI_HP0_awburst; assign s_awburst[1] = S_AXI_HP1_awburst; assign s_awburst[2] = S_AXI_HP2_awburst;
    assign s_awcache[0] = S_AXI_HP0_awcache; assign s_awcache[1] = S_AXI_HP1_awcache; assign s_awcache[2] = S_AXI_HP2_awcache;
    assign s_awid[0] = S_AXI_HP0_awid;       assign s_awid[1] = S_AXI_HP1_awid;       assign s_awid[2] = S_AXI_HP2_awid;
    assign s_awlen[0] = S_AXI_HP0_awlen;     assign s_awlen[1] = S_AXI_HP1_awlen;     assign s_awlen[2] = S_AXI_HP2_awlen;
    assign s_awlock[0] = S_AXI_HP0_awlock;   assign s_awlock[1] = S_AXI_HP1_awlock;   assign s_awlock[2] = S_AXI_HP2_awlock;
    assign s_awprot[0] = S_AXI_HP0_awprot;   assign s_awprot[1] = S_AXI_HP1_awprot;   assign s_awprot[2] = S_AXI_HP2_awprot;
    assign s_awqos[0] = S_AXI_HP0_awqos;     assign s_awqos[1] = S_AXI_HP1_awqos;     assign s_awqos[2] = S_AXI_HP2_awqos;
    assign s_awsize[0] = S_AXI_HP0_awsize;   assign s_awsize[1] = S_AXI_HP1_awsize;   assign s_awsize[2] = S_AXI_HP2_awsize;
    assign s_awvalid[0] = S_AXI_HP0_awvalid; assign s_awvalid[1] = S_AXI_HP1_awvalid; assign s_awvalid[2] = S_AXI_HP2_awvalid;
    assign s_wdata[0] = S_AXI_HP0_wdata;     assign s_wdata[1] = S_AXI_HP1_wdata;     assign s_wdata[2] = S_AXI_HP2_wdata;
    assign s_wid[0] = S_AXI_HP0_wid;         assign s_wid[1] = S_AXI_HP1_wid;         assign s_wid[2] = S_AXI_HP2_wid;
    assign s_wlast[0] = S_AXI_HP0_wlast;     assign s_wlast[1] = S_AXI_HP1_wlast;     assign s_wlast[2] = S_AXI_HP2_wlast;
    assign s_wstrb[0] = S_AXI_HP0_wstrb;     assign s_wstrb[1] = S_AXI_HP1_wstrb;     assign s_wstrb[2] = S_AXI_HP2_wstrb;
    assign s_wvalid[0] = S_AXI_HP0_wvalid;   assign s_wvalid[1] = S_AXI_HP1_wvalid;   assign s_wvalid[2] = S_AXI_HP2_wvalid;
    assign s_bready[0] = S_AXI_HP0_bready;   assign s_bready[1] = S_AXI_HP1_bready;   assign s_bready[2] = S_AXI_HP2_bready;

    assign S_AXI_HP0_arready = s_arready[0]; assign S_AXI_HP1_arready = s_arready[1]; assign S_AXI_HP2_arready = s_arready[2];
    assign S_AXI_HP0_awready = s_awready[0]; assign S_AXI_HP1_awready = s_awready[1]; assign S_AXI_HP2_awready = s_awready[2];
    assign S_AXI_HP0_wready  = s_wready[0];  assign S_AXI_HP1_wready  = s_wready[1];  assign S_AXI_HP2_wready  = s_wready[2];
    assign S_AXI_HP0_bvalid  = s_bvalid[0];  assign S_AXI_HP1_bvalid  = s_bvalid[1];  assign S_AXI_HP2_bvalid  = s_bvalid[2];
    assign S_AXI_HP0_bid     = s_bid[0];     assign S_AXI_HP1_bid     = s_bid[1];     assign S_AXI_HP2_bid     = s_bid[2];
    assign S_AXI_HP0_bresp   = 2'b00;        assign S_AXI_HP1_bresp   = 2'b00;        assign S_AXI_HP2_bresp   = 2'b00;
    assign S_AXI_HP0_rvalid  = s_rvalid[0];  assign S_AXI_HP1_rvalid  = s_rvalid[1];  assign S_AXI_HP2_rvalid  = s_rvalid[2];
    assign S_AXI_HP0_rdata   = s_rdata[0];   assign S_AXI_HP1_rdata   = s_rdata[1];   assign S_AXI_HP2_rdata   = s_rdata[2];
    assign S_AXI_HP0_rlast   = s_rlast[0];   assign S_AXI_HP1_rlast   = s_rlast[1];   assign S_AXI_HP2_rlast   = s_rlast[2];
    assign S_AXI_HP0_rid     = s_rid[0];     assign S_AXI_HP1_rid     = s_rid[1];     assign S_AXI_HP2_rid     = s_rid[2];
    assign S_AXI_HP0_rresp   = 2'b00;        assign S_AXI_HP1_rresp   = 2'b00;        assign S_AXI_HP2_rresp   = 2'b00;

    genvar p;
    generate
        for (p = 0; p < 3; p = p + 1) begin : hp
            localparam WRITE_OK = (p == 1);     // SPEC: HP1 writes, HP0/HP2 read
            reg [15:0] lfsr = 16'hACE1 + p * 16'h1111;
            wire       rnd_a = (STALLS == 0) || (lfsr[2:0] != 3'd0);
            wire       rnd_w = (STALLS == 0) || (lfsr[5:3] != 3'd0);
            wire       rnd_r = (STALLS == 0) || (lfsr[8:6] != 3'd0);

            // ---- write channel ----
            reg [1:0]  wst = 2'd0;              // 0 AW, 1 W beats, 2 B
            reg [31:0] wa;
            reg [3:0]  wl, wb;
            reg [5:0]  wi;
            // ---- read channel ----
            reg [1:0]  rst_ = 2'd0;             // 0 AR, 1 latency, 2 R beats
            reg [31:0] ra;
            reg [3:0]  rl, rb;
            reg [5:0]  ri;
            reg        rv = 1'b0;
            integer    lat;
            // ---- previous-cycle handshake state for stability checks ----
            reg        p_arv = 0, p_arr = 0, p_awv = 0, p_awr = 0, p_wv = 0, p_wr = 0;
            reg [31:0] p_araddr, p_awaddr;
            reg [3:0]  p_arlen, p_awlen;
            reg [63:0] p_wdata;
            reg        p_wlast;

            assign s_awready[p] = (wst == 2'd0) && rnd_a;
            assign s_wready[p]  = (wst == 2'd1) && rnd_w;
            assign s_bvalid[p]  = (wst == 2'd2);
            assign s_bid[p]     = wi;
            assign s_arready[p] = (rst_ == 2'd0) && rnd_a;
            assign s_rvalid[p]  = rv;
            assign s_rdata[p]   = ddr[ra[MEM_AW+2:3]];
            assign s_rlast[p]   = (rb == rl);
            assign s_rid[p]     = ri;

            always @(posedge s_aclk[p]) begin
                lfsr <= {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};

                // stability: VALID must stay high with a stable payload until READY
                if (p_arv && !p_arr && (!s_arvalid[p] || s_araddr[p] != p_araddr || s_arlen[p] != p_arlen))
                    stub_err("HP AR valid/payload changed before ARREADY");
                if (p_awv && !p_awr && (!s_awvalid[p] || s_awaddr[p] != p_awaddr || s_awlen[p] != p_awlen))
                    stub_err("HP AW valid/payload changed before AWREADY");
                if (p_wv && !p_wr && (!s_wvalid[p] || s_wdata[p] != p_wdata || s_wlast[p] != p_wlast))
                    stub_err("HP W valid/payload changed before WREADY");
                p_arv <= s_arvalid[p]; p_arr <= s_arready[p]; p_araddr <= s_araddr[p]; p_arlen <= s_arlen[p];
                p_awv <= s_awvalid[p]; p_awr <= s_awready[p]; p_awaddr <= s_awaddr[p]; p_awlen <= s_awlen[p];
                p_wv  <= s_wvalid[p];  p_wr  <= s_wready[p];  p_wdata  <= s_wdata[p];  p_wlast <= s_wlast[p];

                // ---- write ----
                case (wst)
                    2'd0: if (s_awvalid[p] && s_awready[p]) begin
                        if (!WRITE_OK) stub_err("write address on a read-only HP port (HP0/HP2)");
                        if (s_awsize[p] != 3'd3 || s_awburst[p] != 2'b01 || s_awid[p] != 6'd0 ||
                            s_awcache[p] != 4'b0011 || s_awprot[p] != 3'd0 || s_awlock[p] != 2'd0 ||
                            s_awqos[p] != 4'd0)
                            stub_err("HP AW constant fields (size/burst/id/cache/prot/lock/qos)");
                        if (s_awaddr[p][2:0] != 3'd0) stub_err("HP AW address not 8-byte aligned");
                        if ({1'b0, s_awaddr[p][11:0]} + ({9'd0, s_awlen[p]} + 13'd1) * 13'd8 > 13'd4096)
                            stub_err("HP write burst crosses a 4 KB boundary");
                        wa <= s_awaddr[p]; wl <= s_awlen[p]; wb <= 4'd0; wi <= s_awid[p];
                        wst <= 2'd1;
                    end
                    2'd1: if (s_wvalid[p] && s_wready[p]) begin
                        if (s_wstrb[p] != 8'hFF) stub_err("HP WSTRB != FF");
                        if (s_wid[p] != wi)      stub_err("HP WID != AWID");
                        if (s_wlast[p] != (wb == wl)) stub_err("HP WLAST not on the last beat");
                        for (i = 0; i < 8; i = i + 1)
                            if (s_wstrb[p][i])
                                ddr[wa[MEM_AW+2:3]][i*8 +: 8] = s_wdata[p][i*8 +: 8];
                        hp_wr_beats = hp_wr_beats + 1;
                        wa <= wa + 32'd8;
                        wb <= wb + 4'd1;
                        if (s_wlast[p]) wst <= 2'd2;
                    end
                    default: if (s_bready[p]) wst <= 2'd0;
                endcase

                // ---- read ----
                case (rst_)
                    2'd0: if (s_arvalid[p] && s_arready[p]) begin
                        if (WRITE_OK) stub_err("read address on the write-only HP port (HP1)");
                        if (s_arsize[p] != 3'd3 || s_arburst[p] != 2'b01 || s_arid[p] != 6'd0 ||
                            s_arcache[p] != 4'b0011 || s_arprot[p] != 3'd0 || s_arlock[p] != 2'd0 ||
                            s_arqos[p] != 4'd0)
                            stub_err("HP AR constant fields (size/burst/id/cache/prot/lock/qos)");
                        if (s_araddr[p][2:0] != 3'd0) stub_err("HP AR address not 8-byte aligned");
                        if ({1'b0, s_araddr[p][11:0]} + ({9'd0, s_arlen[p]} + 13'd1) * 13'd8 > 13'd4096)
                            stub_err("HP read burst crosses a 4 KB boundary");
                        ra <= s_araddr[p]; rl <= s_arlen[p]; rb <= 4'd0; ri <= s_arid[p];
                        lat <= RD_LATENCY;
                        rst_ <= 2'd1;
                    end
                    2'd1: if (lat <= 1) rst_ <= 2'd2; else lat <= lat - 1;
                    default: begin
                        if (!rv) begin
                            if (rnd_r) rv <= 1'b1;
                        end else if (s_rready[p]) begin
                            hp_rd_beats = hp_rd_beats + 1;
                            if (rb == rl) begin
                                rv   <= 1'b0;
                                rst_ <= 2'd0;
                            end else begin
                                rb <= rb + 4'd1;
                                ra <= ra + 32'd8;
                                rv <= rnd_r;
                            end
                        end
                    end
                endcase
            end
        end
    endgenerate
`endif
endmodule
/* verilator lint_on UNUSED */
