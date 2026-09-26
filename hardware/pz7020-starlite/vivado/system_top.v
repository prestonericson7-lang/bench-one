// system_top.v -- top level of the one PZ7020-StarLite bitstream (build_system.tcl).
//
// The Zynq holds one PL image, so everything the PL does is in this design:
//   u_sys  system_wrapper (the block design): PS7 (runtime DDR setup is ps7/: 512 MB 16-bit), pl_regs @0x4000_0000,
//          AXI DMA @0x4040_0000 + zaccel_gemv (matrix engine, S_AXI_HP3, FCLK0 100 MHz),
//          GEM1 -> GMII-to-RGMII -> PL PHY (eth1). The GPU's register window 0x43C0_0000/4K
//          leaves the GP0 interconnect as port M_AXI_GPU; S_AXI_HP0/1/2 are the GPU's DDR3 ports.
//   u_gpu  gpu_pl (accel/gpu/rtl): the FPGA-GPU -- its own MMCM from the 50 MHz U18 clock; its
//          core clock (148.75 MHz) clocks M_AXI_GPU and S_AXI_HP0/1/2 (port gpu_aclk).
// LEDs: the GPU drives LED1 (heartbeat) and LED2 (frame toggle); pl_regs' LED outputs are left
// unconnected in the block design. Keys and the fan stay with pl_regs.
module system_top (
    // ---- PS7 DDR3 and fixed IO ----
    inout  wire [14:0] DDR_addr,
    inout  wire [2:0]  DDR_ba,
    inout  wire        DDR_cas_n,
    inout  wire        DDR_ck_n,
    inout  wire        DDR_ck_p,
    inout  wire        DDR_cke,
    inout  wire        DDR_cs_n,
    inout  wire [3:0]  DDR_dm,
    inout  wire [31:0] DDR_dq,
    inout  wire [3:0]  DDR_dqs_n,
    inout  wire [3:0]  DDR_dqs_p,
    inout  wire        DDR_odt,
    inout  wire        DDR_ras_n,
    inout  wire        DDR_reset_n,
    inout  wire        DDR_we_n,
    inout  wire        FIXED_IO_ddr_vrn,
    inout  wire        FIXED_IO_ddr_vrp,
    inout  wire [53:0] FIXED_IO_mio,
    inout  wire        FIXED_IO_ps_clk,
    inout  wire        FIXED_IO_ps_porb,
    inout  wire        FIXED_IO_ps_srstb,
    // ---- pl_regs board I/O ----
    output wire        fan_pwm,
    input  wire        fan_tach,
    input  wire        key1_n,
    input  wire        key2_n,
    // ---- PL Ethernet PHY (GEM1 via GMII-to-RGMII) ----
    output wire        mdio_phy_mdc,
    inout  wire        mdio_phy_mdio_io,
    input  wire [3:0]  rgmii_rd,
    input  wire        rgmii_rx_ctl,
    input  wire        rgmii_rxc,
    output wire [3:0]  rgmii_td,
    output wire        rgmii_tx_ctl,
    output wire        rgmii_txc,
    // ---- FPGA-GPU pins (accel/gpu/SPEC.md section 2) ----
    input  wire        clk50,
    output wire        led1,
    output wire        led2,
    output wire        hdmi_d0_p,
    output wire        hdmi_d0_n,
    output wire        hdmi_d1_p,
    output wire        hdmi_d1_n,
    output wire        hdmi_d2_p,
    output wire        hdmi_d2_n,
    output wire        hdmi_clk_p,
    output wire        hdmi_clk_n,
    output wire        hdmi_out_en,
    input  wire        hdmi_hpd,
    input  wire [15:0] tb_d,
    input  wire        tb_sor,
    input  wire        tb_strobe,
    output wire        tb_busy
);
    wire gpu_clk;                              // GPU core clock (148.75 MHz)
    wire [1:0] gpu_led;
    assign led1 = gpu_led[0];                  // LED1 R19: heartbeat
    assign led2 = gpu_led[1];                  // LED2 V13: toggles every 30 rendered frames

    // GP0 window 0x43C0_0000 (block design port M_AXI_GPU -> GPU registers)
    wire [31:0] gp_araddr;
    wire [1:0]  gp_arburst;
    wire [3:0]  gp_arcache;
    wire [11:0] gp_arid;
    wire [3:0]  gp_arlen;
    wire [1:0]  gp_arlock;
    wire [2:0]  gp_arprot;
    wire [3:0]  gp_arqos;
    wire        gp_arready;
    wire [2:0]  gp_arsize;
    wire        gp_arvalid;
    wire [31:0] gp_awaddr;
    wire [1:0]  gp_awburst;
    wire [3:0]  gp_awcache;
    wire [11:0] gp_awid;
    wire [3:0]  gp_awlen;
    wire [1:0]  gp_awlock;
    wire [2:0]  gp_awprot;
    wire [3:0]  gp_awqos;
    wire        gp_awready;
    wire [2:0]  gp_awsize;
    wire        gp_awvalid;
    wire [11:0] gp_bid;
    wire        gp_bready;
    wire [1:0]  gp_bresp;
    wire        gp_bvalid;
    wire [31:0] gp_rdata;
    wire [11:0] gp_rid;
    wire        gp_rlast;
    wire        gp_rready;
    wire [1:0]  gp_rresp;
    wire        gp_rvalid;
    wire [31:0] gp_wdata;
    wire [11:0] gp_wid;
    wire        gp_wlast;
    wire        gp_wready;
    wire [3:0]  gp_wstrb;
    wire        gp_wvalid;
    // HP0 (GPU scanout)
    wire [31:0] hp0_araddr;
    wire [1:0]  hp0_arburst;
    wire [3:0]  hp0_arcache;
    wire [5:0]  hp0_arid;
    wire [3:0]  hp0_arlen;
    wire [1:0]  hp0_arlock;
    wire [2:0]  hp0_arprot;
    wire [3:0]  hp0_arqos;
    wire        hp0_arready;
    wire [2:0]  hp0_arsize;
    wire        hp0_arvalid;
    wire [31:0] hp0_awaddr;
    wire [1:0]  hp0_awburst;
    wire [3:0]  hp0_awcache;
    wire [5:0]  hp0_awid;
    wire [3:0]  hp0_awlen;
    wire [1:0]  hp0_awlock;
    wire [2:0]  hp0_awprot;
    wire [3:0]  hp0_awqos;
    wire        hp0_awready;
    wire [2:0]  hp0_awsize;
    wire        hp0_awvalid;
    wire [5:0]  hp0_bid;
    wire        hp0_bready;
    wire [1:0]  hp0_bresp;
    wire        hp0_bvalid;
    wire [63:0] hp0_rdata;
    wire [5:0]  hp0_rid;
    wire        hp0_rlast;
    wire        hp0_rready;
    wire [1:0]  hp0_rresp;
    wire        hp0_rvalid;
    wire [63:0] hp0_wdata;
    wire [5:0]  hp0_wid;
    wire        hp0_wlast;
    wire        hp0_wready;
    wire [7:0]  hp0_wstrb;
    wire        hp0_wvalid;
    // HP1 (GPU strip writer)
    wire [31:0] hp1_araddr;
    wire [1:0]  hp1_arburst;
    wire [3:0]  hp1_arcache;
    wire [5:0]  hp1_arid;
    wire [3:0]  hp1_arlen;
    wire [1:0]  hp1_arlock;
    wire [2:0]  hp1_arprot;
    wire [3:0]  hp1_arqos;
    wire        hp1_arready;
    wire [2:0]  hp1_arsize;
    wire        hp1_arvalid;
    wire [31:0] hp1_awaddr;
    wire [1:0]  hp1_awburst;
    wire [3:0]  hp1_awcache;
    wire [5:0]  hp1_awid;
    wire [3:0]  hp1_awlen;
    wire [1:0]  hp1_awlock;
    wire [2:0]  hp1_awprot;
    wire [3:0]  hp1_awqos;
    wire        hp1_awready;
    wire [2:0]  hp1_awsize;
    wire        hp1_awvalid;
    wire [5:0]  hp1_bid;
    wire        hp1_bready;
    wire [1:0]  hp1_bresp;
    wire        hp1_bvalid;
    wire [63:0] hp1_rdata;
    wire [5:0]  hp1_rid;
    wire        hp1_rlast;
    wire        hp1_rready;
    wire [1:0]  hp1_rresp;
    wire        hp1_rvalid;
    wire [63:0] hp1_wdata;
    wire [5:0]  hp1_wid;
    wire        hp1_wlast;
    wire        hp1_wready;
    wire [7:0]  hp1_wstrb;
    wire        hp1_wvalid;
    // HP2 (GPU sprite reader)
    wire [31:0] hp2_araddr;
    wire [1:0]  hp2_arburst;
    wire [3:0]  hp2_arcache;
    wire [5:0]  hp2_arid;
    wire [3:0]  hp2_arlen;
    wire [1:0]  hp2_arlock;
    wire [2:0]  hp2_arprot;
    wire [3:0]  hp2_arqos;
    wire        hp2_arready;
    wire [2:0]  hp2_arsize;
    wire        hp2_arvalid;
    wire [31:0] hp2_awaddr;
    wire [1:0]  hp2_awburst;
    wire [3:0]  hp2_awcache;
    wire [5:0]  hp2_awid;
    wire [3:0]  hp2_awlen;
    wire [1:0]  hp2_awlock;
    wire [2:0]  hp2_awprot;
    wire [3:0]  hp2_awqos;
    wire        hp2_awready;
    wire [2:0]  hp2_awsize;
    wire        hp2_awvalid;
    wire [5:0]  hp2_bid;
    wire        hp2_bready;
    wire [1:0]  hp2_bresp;
    wire        hp2_bvalid;
    wire [63:0] hp2_rdata;
    wire [5:0]  hp2_rid;
    wire        hp2_rlast;
    wire        hp2_rready;
    wire [1:0]  hp2_rresp;
    wire        hp2_rvalid;
    wire [63:0] hp2_wdata;
    wire [5:0]  hp2_wid;
    wire        hp2_wlast;
    wire        hp2_wready;
    wire [7:0]  hp2_wstrb;
    wire        hp2_wvalid;

    system_wrapper u_sys (
        .DDR_addr                  (DDR_addr),
        .DDR_ba                    (DDR_ba),
        .DDR_cas_n                 (DDR_cas_n),
        .DDR_ck_n                  (DDR_ck_n),
        .DDR_ck_p                  (DDR_ck_p),
        .DDR_cke                   (DDR_cke),
        .DDR_cs_n                  (DDR_cs_n),
        .DDR_dm                    (DDR_dm),
        .DDR_dq                    (DDR_dq),
        .DDR_dqs_n                 (DDR_dqs_n),
        .DDR_dqs_p                 (DDR_dqs_p),
        .DDR_odt                   (DDR_odt),
        .DDR_ras_n                 (DDR_ras_n),
        .DDR_reset_n               (DDR_reset_n),
        .DDR_we_n                  (DDR_we_n),
        .FIXED_IO_ddr_vrn          (FIXED_IO_ddr_vrn),
        .FIXED_IO_ddr_vrp          (FIXED_IO_ddr_vrp),
        .FIXED_IO_mio              (FIXED_IO_mio),
        .FIXED_IO_ps_clk           (FIXED_IO_ps_clk),
        .FIXED_IO_ps_porb          (FIXED_IO_ps_porb),
        .FIXED_IO_ps_srstb         (FIXED_IO_ps_srstb),
        .fan_pwm                   (fan_pwm),
        .fan_tach                  (fan_tach),
        .key1_n                    (key1_n),
        .key2_n                    (key2_n),
        .mdio_phy_mdc              (mdio_phy_mdc),
        .mdio_phy_mdio_io          (mdio_phy_mdio_io),
        .rgmii_rd                  (rgmii_rd),
        .rgmii_rx_ctl              (rgmii_rx_ctl),
        .rgmii_rxc                 (rgmii_rxc),
        .rgmii_td                  (rgmii_td),
        .rgmii_tx_ctl              (rgmii_tx_ctl),
        .rgmii_txc                 (rgmii_txc),
        .gpu_aclk                  (gpu_clk),
        // ---- GPU register window ----
        .M_AXI_GPU_araddr           (gp_araddr),
        .M_AXI_GPU_arburst          (gp_arburst),
        .M_AXI_GPU_arcache          (gp_arcache),
        .M_AXI_GPU_arid             (gp_arid),
        .M_AXI_GPU_arlen            (gp_arlen),
        .M_AXI_GPU_arlock           (gp_arlock),
        .M_AXI_GPU_arprot           (gp_arprot),
        .M_AXI_GPU_arqos            (gp_arqos),
        .M_AXI_GPU_arready          (gp_arready),
        .M_AXI_GPU_arsize           (gp_arsize),
        .M_AXI_GPU_arvalid          (gp_arvalid),
        .M_AXI_GPU_awaddr           (gp_awaddr),
        .M_AXI_GPU_awburst          (gp_awburst),
        .M_AXI_GPU_awcache          (gp_awcache),
        .M_AXI_GPU_awid             (gp_awid),
        .M_AXI_GPU_awlen            (gp_awlen),
        .M_AXI_GPU_awlock           (gp_awlock),
        .M_AXI_GPU_awprot           (gp_awprot),
        .M_AXI_GPU_awqos            (gp_awqos),
        .M_AXI_GPU_awready          (gp_awready),
        .M_AXI_GPU_awsize           (gp_awsize),
        .M_AXI_GPU_awvalid          (gp_awvalid),
        .M_AXI_GPU_bid              (gp_bid),
        .M_AXI_GPU_bready           (gp_bready),
        .M_AXI_GPU_bresp            (gp_bresp),
        .M_AXI_GPU_bvalid           (gp_bvalid),
        .M_AXI_GPU_rdata            (gp_rdata),
        .M_AXI_GPU_rid              (gp_rid),
        .M_AXI_GPU_rlast            (gp_rlast),
        .M_AXI_GPU_rready           (gp_rready),
        .M_AXI_GPU_rresp            (gp_rresp),
        .M_AXI_GPU_rvalid           (gp_rvalid),
        .M_AXI_GPU_wdata            (gp_wdata),
        .M_AXI_GPU_wid              (gp_wid),
        .M_AXI_GPU_wlast            (gp_wlast),
        .M_AXI_GPU_wready           (gp_wready),
        .M_AXI_GPU_wstrb            (gp_wstrb),
        .M_AXI_GPU_wvalid           (gp_wvalid),
        // ---- HP0 ----
        .S_AXI_HP0_araddr           (hp0_araddr),
        .S_AXI_HP0_arburst          (hp0_arburst),
        .S_AXI_HP0_arcache          (hp0_arcache),
        .S_AXI_HP0_arid             (hp0_arid),
        .S_AXI_HP0_arlen            (hp0_arlen),
        .S_AXI_HP0_arlock           (hp0_arlock),
        .S_AXI_HP0_arprot           (hp0_arprot),
        .S_AXI_HP0_arqos            (hp0_arqos),
        .S_AXI_HP0_arready          (hp0_arready),
        .S_AXI_HP0_arsize           (hp0_arsize),
        .S_AXI_HP0_arvalid          (hp0_arvalid),
        .S_AXI_HP0_awaddr           (hp0_awaddr),
        .S_AXI_HP0_awburst          (hp0_awburst),
        .S_AXI_HP0_awcache          (hp0_awcache),
        .S_AXI_HP0_awid             (hp0_awid),
        .S_AXI_HP0_awlen            (hp0_awlen),
        .S_AXI_HP0_awlock           (hp0_awlock),
        .S_AXI_HP0_awprot           (hp0_awprot),
        .S_AXI_HP0_awqos            (hp0_awqos),
        .S_AXI_HP0_awready          (hp0_awready),
        .S_AXI_HP0_awsize           (hp0_awsize),
        .S_AXI_HP0_awvalid          (hp0_awvalid),
        .S_AXI_HP0_bid              (hp0_bid),
        .S_AXI_HP0_bready           (hp0_bready),
        .S_AXI_HP0_bresp            (hp0_bresp),
        .S_AXI_HP0_bvalid           (hp0_bvalid),
        .S_AXI_HP0_rdata            (hp0_rdata),
        .S_AXI_HP0_rid              (hp0_rid),
        .S_AXI_HP0_rlast            (hp0_rlast),
        .S_AXI_HP0_rready           (hp0_rready),
        .S_AXI_HP0_rresp            (hp0_rresp),
        .S_AXI_HP0_rvalid           (hp0_rvalid),
        .S_AXI_HP0_wdata            (hp0_wdata),
        .S_AXI_HP0_wid              (hp0_wid),
        .S_AXI_HP0_wlast            (hp0_wlast),
        .S_AXI_HP0_wready           (hp0_wready),
        .S_AXI_HP0_wstrb            (hp0_wstrb),
        .S_AXI_HP0_wvalid           (hp0_wvalid),
        // ---- HP1 ----
        .S_AXI_HP1_araddr           (hp1_araddr),
        .S_AXI_HP1_arburst          (hp1_arburst),
        .S_AXI_HP1_arcache          (hp1_arcache),
        .S_AXI_HP1_arid             (hp1_arid),
        .S_AXI_HP1_arlen            (hp1_arlen),
        .S_AXI_HP1_arlock           (hp1_arlock),
        .S_AXI_HP1_arprot           (hp1_arprot),
        .S_AXI_HP1_arqos            (hp1_arqos),
        .S_AXI_HP1_arready          (hp1_arready),
        .S_AXI_HP1_arsize           (hp1_arsize),
        .S_AXI_HP1_arvalid          (hp1_arvalid),
        .S_AXI_HP1_awaddr           (hp1_awaddr),
        .S_AXI_HP1_awburst          (hp1_awburst),
        .S_AXI_HP1_awcache          (hp1_awcache),
        .S_AXI_HP1_awid             (hp1_awid),
        .S_AXI_HP1_awlen            (hp1_awlen),
        .S_AXI_HP1_awlock           (hp1_awlock),
        .S_AXI_HP1_awprot           (hp1_awprot),
        .S_AXI_HP1_awqos            (hp1_awqos),
        .S_AXI_HP1_awready          (hp1_awready),
        .S_AXI_HP1_awsize           (hp1_awsize),
        .S_AXI_HP1_awvalid          (hp1_awvalid),
        .S_AXI_HP1_bid              (hp1_bid),
        .S_AXI_HP1_bready           (hp1_bready),
        .S_AXI_HP1_bresp            (hp1_bresp),
        .S_AXI_HP1_bvalid           (hp1_bvalid),
        .S_AXI_HP1_rdata            (hp1_rdata),
        .S_AXI_HP1_rid              (hp1_rid),
        .S_AXI_HP1_rlast            (hp1_rlast),
        .S_AXI_HP1_rready           (hp1_rready),
        .S_AXI_HP1_rresp            (hp1_rresp),
        .S_AXI_HP1_rvalid           (hp1_rvalid),
        .S_AXI_HP1_wdata            (hp1_wdata),
        .S_AXI_HP1_wid              (hp1_wid),
        .S_AXI_HP1_wlast            (hp1_wlast),
        .S_AXI_HP1_wready           (hp1_wready),
        .S_AXI_HP1_wstrb            (hp1_wstrb),
        .S_AXI_HP1_wvalid           (hp1_wvalid),
        // ---- HP2 ----
        .S_AXI_HP2_araddr           (hp2_araddr),
        .S_AXI_HP2_arburst          (hp2_arburst),
        .S_AXI_HP2_arcache          (hp2_arcache),
        .S_AXI_HP2_arid             (hp2_arid),
        .S_AXI_HP2_arlen            (hp2_arlen),
        .S_AXI_HP2_arlock           (hp2_arlock),
        .S_AXI_HP2_arprot           (hp2_arprot),
        .S_AXI_HP2_arqos            (hp2_arqos),
        .S_AXI_HP2_arready          (hp2_arready),
        .S_AXI_HP2_arsize           (hp2_arsize),
        .S_AXI_HP2_arvalid          (hp2_arvalid),
        .S_AXI_HP2_awaddr           (hp2_awaddr),
        .S_AXI_HP2_awburst          (hp2_awburst),
        .S_AXI_HP2_awcache          (hp2_awcache),
        .S_AXI_HP2_awid             (hp2_awid),
        .S_AXI_HP2_awlen            (hp2_awlen),
        .S_AXI_HP2_awlock           (hp2_awlock),
        .S_AXI_HP2_awprot           (hp2_awprot),
        .S_AXI_HP2_awqos            (hp2_awqos),
        .S_AXI_HP2_awready          (hp2_awready),
        .S_AXI_HP2_awsize           (hp2_awsize),
        .S_AXI_HP2_awvalid          (hp2_awvalid),
        .S_AXI_HP2_bid              (hp2_bid),
        .S_AXI_HP2_bready           (hp2_bready),
        .S_AXI_HP2_bresp            (hp2_bresp),
        .S_AXI_HP2_bvalid           (hp2_bvalid),
        .S_AXI_HP2_rdata            (hp2_rdata),
        .S_AXI_HP2_rid              (hp2_rid),
        .S_AXI_HP2_rlast            (hp2_rlast),
        .S_AXI_HP2_rready           (hp2_rready),
        .S_AXI_HP2_rresp            (hp2_rresp),
        .S_AXI_HP2_rvalid           (hp2_rvalid),
        .S_AXI_HP2_wdata            (hp2_wdata),
        .S_AXI_HP2_wid              (hp2_wid),
        .S_AXI_HP2_wlast            (hp2_wlast),
        .S_AXI_HP2_wready           (hp2_wready),
        .S_AXI_HP2_wstrb            (hp2_wstrb),
        .S_AXI_HP2_wvalid           (hp2_wvalid)
    );

    gpu_pl u_gpu (
        .clk50       (clk50),
        .led         (gpu_led),
        .hdmi_d0_p   (hdmi_d0_p),
        .hdmi_d0_n   (hdmi_d0_n),
        .hdmi_d1_p   (hdmi_d1_p),
        .hdmi_d1_n   (hdmi_d1_n),
        .hdmi_d2_p   (hdmi_d2_p),
        .hdmi_d2_n   (hdmi_d2_n),
        .hdmi_clk_p  (hdmi_clk_p),
        .hdmi_clk_n  (hdmi_clk_n),
        .hdmi_out_en (hdmi_out_en),
        .hdmi_hpd    (hdmi_hpd),
        .tb_d        (tb_d),
        .tb_sor      (tb_sor),
        .tb_strobe   (tb_strobe),
        .tb_busy     (tb_busy),
        .aclk        (gpu_clk),
        .gp_araddr                  (gp_araddr),
        .gp_arburst                 (gp_arburst),
        .gp_arcache                 (gp_arcache),
        .gp_arid                    (gp_arid),
        .gp_arlen                   (gp_arlen),
        .gp_arlock                  (gp_arlock),
        .gp_arprot                  (gp_arprot),
        .gp_arqos                   (gp_arqos),
        .gp_arready                 (gp_arready),
        .gp_arsize                  (gp_arsize),
        .gp_arvalid                 (gp_arvalid),
        .gp_awaddr                  (gp_awaddr),
        .gp_awburst                 (gp_awburst),
        .gp_awcache                 (gp_awcache),
        .gp_awid                    (gp_awid),
        .gp_awlen                   (gp_awlen),
        .gp_awlock                  (gp_awlock),
        .gp_awprot                  (gp_awprot),
        .gp_awqos                   (gp_awqos),
        .gp_awready                 (gp_awready),
        .gp_awsize                  (gp_awsize),
        .gp_awvalid                 (gp_awvalid),
        .gp_bid                     (gp_bid),
        .gp_bready                  (gp_bready),
        .gp_bresp                   (gp_bresp),
        .gp_bvalid                  (gp_bvalid),
        .gp_rdata                   (gp_rdata),
        .gp_rid                     (gp_rid),
        .gp_rlast                   (gp_rlast),
        .gp_rready                  (gp_rready),
        .gp_rresp                   (gp_rresp),
        .gp_rvalid                  (gp_rvalid),
        .gp_wdata                   (gp_wdata),
        .gp_wid                     (gp_wid),
        .gp_wlast                   (gp_wlast),
        .gp_wready                  (gp_wready),
        .gp_wstrb                   (gp_wstrb),
        .gp_wvalid                  (gp_wvalid),
        .hp0_araddr                 (hp0_araddr),
        .hp0_arburst                (hp0_arburst),
        .hp0_arcache                (hp0_arcache),
        .hp0_arid                   (hp0_arid),
        .hp0_arlen                  (hp0_arlen),
        .hp0_arlock                 (hp0_arlock),
        .hp0_arprot                 (hp0_arprot),
        .hp0_arqos                  (hp0_arqos),
        .hp0_arready                (hp0_arready),
        .hp0_arsize                 (hp0_arsize),
        .hp0_arvalid                (hp0_arvalid),
        .hp0_awaddr                 (hp0_awaddr),
        .hp0_awburst                (hp0_awburst),
        .hp0_awcache                (hp0_awcache),
        .hp0_awid                   (hp0_awid),
        .hp0_awlen                  (hp0_awlen),
        .hp0_awlock                 (hp0_awlock),
        .hp0_awprot                 (hp0_awprot),
        .hp0_awqos                  (hp0_awqos),
        .hp0_awready                (hp0_awready),
        .hp0_awsize                 (hp0_awsize),
        .hp0_awvalid                (hp0_awvalid),
        .hp0_bid                    (hp0_bid),
        .hp0_bready                 (hp0_bready),
        .hp0_bresp                  (hp0_bresp),
        .hp0_bvalid                 (hp0_bvalid),
        .hp0_rdata                  (hp0_rdata),
        .hp0_rid                    (hp0_rid),
        .hp0_rlast                  (hp0_rlast),
        .hp0_rready                 (hp0_rready),
        .hp0_rresp                  (hp0_rresp),
        .hp0_rvalid                 (hp0_rvalid),
        .hp0_wdata                  (hp0_wdata),
        .hp0_wid                    (hp0_wid),
        .hp0_wlast                  (hp0_wlast),
        .hp0_wready                 (hp0_wready),
        .hp0_wstrb                  (hp0_wstrb),
        .hp0_wvalid                 (hp0_wvalid),
        .hp1_araddr                 (hp1_araddr),
        .hp1_arburst                (hp1_arburst),
        .hp1_arcache                (hp1_arcache),
        .hp1_arid                   (hp1_arid),
        .hp1_arlen                  (hp1_arlen),
        .hp1_arlock                 (hp1_arlock),
        .hp1_arprot                 (hp1_arprot),
        .hp1_arqos                  (hp1_arqos),
        .hp1_arready                (hp1_arready),
        .hp1_arsize                 (hp1_arsize),
        .hp1_arvalid                (hp1_arvalid),
        .hp1_awaddr                 (hp1_awaddr),
        .hp1_awburst                (hp1_awburst),
        .hp1_awcache                (hp1_awcache),
        .hp1_awid                   (hp1_awid),
        .hp1_awlen                  (hp1_awlen),
        .hp1_awlock                 (hp1_awlock),
        .hp1_awprot                 (hp1_awprot),
        .hp1_awqos                  (hp1_awqos),
        .hp1_awready                (hp1_awready),
        .hp1_awsize                 (hp1_awsize),
        .hp1_awvalid                (hp1_awvalid),
        .hp1_bid                    (hp1_bid),
        .hp1_bready                 (hp1_bready),
        .hp1_bresp                  (hp1_bresp),
        .hp1_bvalid                 (hp1_bvalid),
        .hp1_rdata                  (hp1_rdata),
        .hp1_rid                    (hp1_rid),
        .hp1_rlast                  (hp1_rlast),
        .hp1_rready                 (hp1_rready),
        .hp1_rresp                  (hp1_rresp),
        .hp1_rvalid                 (hp1_rvalid),
        .hp1_wdata                  (hp1_wdata),
        .hp1_wid                    (hp1_wid),
        .hp1_wlast                  (hp1_wlast),
        .hp1_wready                 (hp1_wready),
        .hp1_wstrb                  (hp1_wstrb),
        .hp1_wvalid                 (hp1_wvalid),
        .hp2_araddr                 (hp2_araddr),
        .hp2_arburst                (hp2_arburst),
        .hp2_arcache                (hp2_arcache),
        .hp2_arid                   (hp2_arid),
        .hp2_arlen                  (hp2_arlen),
        .hp2_arlock                 (hp2_arlock),
        .hp2_arprot                 (hp2_arprot),
        .hp2_arqos                  (hp2_arqos),
        .hp2_arready                (hp2_arready),
        .hp2_arsize                 (hp2_arsize),
        .hp2_arvalid                (hp2_arvalid),
        .hp2_awaddr                 (hp2_awaddr),
        .hp2_awburst                (hp2_awburst),
        .hp2_awcache                (hp2_awcache),
        .hp2_awid                   (hp2_awid),
        .hp2_awlen                  (hp2_awlen),
        .hp2_awlock                 (hp2_awlock),
        .hp2_awprot                 (hp2_awprot),
        .hp2_awqos                  (hp2_awqos),
        .hp2_awready                (hp2_awready),
        .hp2_awsize                 (hp2_awsize),
        .hp2_awvalid                (hp2_awvalid),
        .hp2_bid                    (hp2_bid),
        .hp2_bready                 (hp2_bready),
        .hp2_bresp                  (hp2_bresp),
        .hp2_bvalid                 (hp2_bvalid),
        .hp2_rdata                  (hp2_rdata),
        .hp2_rid                    (hp2_rid),
        .hp2_rlast                  (hp2_rlast),
        .hp2_rready                 (hp2_rready),
        .hp2_rresp                  (hp2_rresp),
        .hp2_rvalid                 (hp2_rvalid),
        .hp2_wdata                  (hp2_wdata),
        .hp2_wid                    (hp2_wid),
        .hp2_wlast                  (hp2_wlast),
        .hp2_wready                 (hp2_wready),
        .hp2_wstrb                  (hp2_wstrb),
        .hp2_wvalid                 (hp2_wvalid)
    );

endmodule
