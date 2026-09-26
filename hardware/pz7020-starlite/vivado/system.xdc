# system.xdc -- PL pins and timing of the one PZ7020-StarLite system bitstream (build_system.tcl,
# top system_top.v): platform (pl_regs, PL Ethernet) + FPGA-GPU (accel/gpu) + matrix engine.
# Balls: manual Parts 3.5 (PL RGMII), 3.15 (HDMI), 3.16 (LEDs), 3.17 (KEYs); JM1 from the CON pins
# xlsx. Every ball below was checked against ../constraints/pz7020_starlite_board.xdc and is used
# once (build_system.tcl re-checks that no ball is placed twice).
# The PS side (DDR, MIO) is fixed silicon and needs no constraints. The matrix engine and the DMA
# have no pins.
set_property CFGBVS VCCO        [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]

## ---- LEDs: driven by the GPU (led1 = heartbeat, led2 = frame toggle); pl_regs' LED outputs are
## ---- unconnected in this design
set_property -dict {PACKAGE_PIN R19 IOSTANDARD LVCMOS33} [get_ports led1]       ;# LED1 IO_0_34
set_property -dict {PACKAGE_PIN V13 IOSTANDARD LVCMOS33} [get_ports led2]       ;# LED2 IO_L3N_34

## ---- register-file board I/O (pl_regs) ----
set_property -dict {PACKAGE_PIN G14 IOSTANDARD LVCMOS33 PULLUP true} [get_ports key1_n]   ;# KEY1 IO_0_35
set_property -dict {PACKAGE_PIN J15 IOSTANDARD LVCMOS33 PULLUP true} [get_ports key2_n]   ;# KEY2 IO_25_35
set_property -dict {PACKAGE_PIN H16 IOSTANDARD LVCMOS33 SLEW SLOW DRIVE 8} [get_ports fan_pwm]   ;# JM1 pin 5 IO_13P_35
set_property -dict {PACKAGE_PIN H17 IOSTANDARD LVCMOS33 PULLUP true} [get_ports fan_tach] ;# JM1 pin 7 IO_13N_35
set_false_path -from [get_ports {key1_n key2_n fan_tach}]
set_false_path -to   [get_ports {led1 led2 fan_pwm}]

## ---- PL-side Gigabit Ethernet: RTL8211F-CG U16 at MDIO address 2, BANK34 3.3 V  [M 3.5] ----
set_property -dict {PACKAGE_PIN V20 IOSTANDARD LVCMOS33 SLEW FAST} [get_ports rgmii_txc]       ;# PL_GPHY_GTX_CLK IO_L16P_34
set_property -dict {PACKAGE_PIN N20 IOSTANDARD LVCMOS33 SLEW FAST} [get_ports {rgmii_td[0]}]   ;# PL_GPHY_TXD0 IO_L14P_34
set_property -dict {PACKAGE_PIN P20 IOSTANDARD LVCMOS33 SLEW FAST} [get_ports {rgmii_td[1]}]   ;# PL_GPHY_TXD1 IO_L14N_34
set_property -dict {PACKAGE_PIN T20 IOSTANDARD LVCMOS33 SLEW FAST} [get_ports {rgmii_td[2]}]   ;# PL_GPHY_TXD2 IO_L15P_34
set_property -dict {PACKAGE_PIN U20 IOSTANDARD LVCMOS33 SLEW FAST} [get_ports {rgmii_td[3]}]   ;# PL_GPHY_TXD3 IO_L15N_34
set_property -dict {PACKAGE_PIN W20 IOSTANDARD LVCMOS33 SLEW FAST} [get_ports rgmii_tx_ctl]    ;# PL_GPHY_TX_EN IO_L16N_34
set_property -dict {PACKAGE_PIN N18 IOSTANDARD LVCMOS33} [get_ports rgmii_rxc]                 ;# PL_GPHY_RX_CLK IO_L13P_34 (MRCC)
set_property -dict {PACKAGE_PIN Y18 IOSTANDARD LVCMOS33} [get_ports {rgmii_rd[0]}]             ;# PL_GPHY_RXD0 IO_L17P_34
set_property -dict {PACKAGE_PIN Y19 IOSTANDARD LVCMOS33} [get_ports {rgmii_rd[1]}]             ;# PL_GPHY_RXD1 IO_L17N_34
set_property -dict {PACKAGE_PIN V16 IOSTANDARD LVCMOS33} [get_ports {rgmii_rd[2]}]             ;# PL_GPHY_RXD2 IO_L18P_34
set_property -dict {PACKAGE_PIN W16 IOSTANDARD LVCMOS33} [get_ports {rgmii_rd[3]}]             ;# PL_GPHY_RXD3 IO_L18N_34
set_property -dict {PACKAGE_PIN P19 IOSTANDARD LVCMOS33} [get_ports rgmii_rx_ctl]              ;# PL_GPHY_RX_DV IO_L13N_34
set_property -dict {PACKAGE_PIN U14 IOSTANDARD LVCMOS33} [get_ports mdio_phy_mdc]              ;# PL_GPHY_MDC IO_L11P_34
set_property -dict {PACKAGE_PIN U15 IOSTANDARD LVCMOS33} [get_ports mdio_phy_mdio_io]          ;# PL_GPHY_MDIO IO_L11N_34

## The GMII-to-RGMII core's own XDC reads the RX clock off this port (PG160: the top level defines it);
## without it the 18 RX capture registers are unclocked and never timed.
create_clock -period 8.000 -name rgmii_rxc [get_ports rgmii_rxc]
## MDIO is a ~2.5 MHz management bus sampled by the converter's own logic
set_false_path -to   [get_ports {mdio_phy_mdc mdio_phy_mdio_io}]
set_false_path -from [get_ports mdio_phy_mdio_io]

## =============================================================================================
## FPGA-GPU (accel/gpu/SPEC.md section 2; from accel/gpu/fpga/pz7020_gpu.xdc, instance u_gpu)
## =============================================================================================
## ---- 50 MHz PL oscillator Y2 -> the GPU's MMCM ----
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports clk50]     ;# PL_CLK_50M IO_12P_MRCC_34
create_clock -period 20.000 -name clk50 [get_ports clk50]

## ---- HDMI (DVI signalling), BANK34. TMDS_33 needs the sink's 50 ohm pull-ups to 3.3 V [M 3.15]
set_property -dict {PACKAGE_PIN V17 IOSTANDARD TMDS_33} [get_ports hdmi_d0_p]   ;# HDMI_DATA0_P IO_L21P_34
set_property -dict {PACKAGE_PIN V18 IOSTANDARD TMDS_33} [get_ports hdmi_d0_n]   ;# HDMI_DATA0_N IO_L21N_34
set_property -dict {PACKAGE_PIN W18 IOSTANDARD TMDS_33} [get_ports hdmi_d1_p]   ;# HDMI_DATA1_P IO_L22P_34
set_property -dict {PACKAGE_PIN W19 IOSTANDARD TMDS_33} [get_ports hdmi_d1_n]   ;# HDMI_DATA1_N IO_L22N_34
set_property -dict {PACKAGE_PIN N17 IOSTANDARD TMDS_33} [get_ports hdmi_d2_p]   ;# HDMI_DATA2_P IO_L23P_34
set_property -dict {PACKAGE_PIN P18 IOSTANDARD TMDS_33} [get_ports hdmi_d2_n]   ;# HDMI_DATA2_N IO_L23N_34
set_property -dict {PACKAGE_PIN T17 IOSTANDARD TMDS_33} [get_ports hdmi_clk_p]  ;# HDMI_CLK_P   IO_L20P_34
set_property -dict {PACKAGE_PIN R18 IOSTANDARD TMDS_33} [get_ports hdmi_clk_n]  ;# HDMI_CLK_N   IO_L20N_34
set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS33} [get_ports hdmi_out_en] ;# HDMI_OUT_EN IO_L24N_34
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS33} [get_ports hdmi_hpd]    ;# HDMI_HPD    IO_L24P_34

## ---- Teensy 4.1 parallel bus on JM1 (GPU SPEC section 3), BANK35. tb_d[i] = JM1 pin 9+i.
## Inputs get a weak PULLDOWN: the Teensy keeps its pins high-Z until it has seen BUSY = 0 for
## 10 ms, so without a pull a floating STROBE could produce spurious transfers; a pull-down never
## back-powers an unpowered Teensy. (JM1 pins 5/7 = H16/H17 are the fan, above.)
set_property -dict {PACKAGE_PIN E18 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[0]}]   ;# JM1 pin 9
set_property -dict {PACKAGE_PIN F16 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[1]}]   ;# JM1 pin 10
set_property -dict {PACKAGE_PIN E19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[2]}]   ;# JM1 pin 11
set_property -dict {PACKAGE_PIN F17 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[3]}]   ;# JM1 pin 12
set_property -dict {PACKAGE_PIN G17 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[4]}]   ;# JM1 pin 13
set_property -dict {PACKAGE_PIN B19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[5]}]   ;# JM1 pin 14
set_property -dict {PACKAGE_PIN G18 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[6]}]   ;# JM1 pin 15
set_property -dict {PACKAGE_PIN A20 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[7]}]   ;# JM1 pin 16
set_property -dict {PACKAGE_PIN D19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[8]}]   ;# JM1 pin 17
set_property -dict {PACKAGE_PIN C20 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[9]}]   ;# JM1 pin 18
set_property -dict {PACKAGE_PIN D20 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[10]}]  ;# JM1 pin 19
set_property -dict {PACKAGE_PIN B20 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[11]}]  ;# JM1 pin 20
set_property -dict {PACKAGE_PIN J18 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[12]}]  ;# JM1 pin 21
set_property -dict {PACKAGE_PIN K19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[13]}]  ;# JM1 pin 22
set_property -dict {PACKAGE_PIN H18 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[14]}]  ;# JM1 pin 23
set_property -dict {PACKAGE_PIN J19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[15]}]  ;# JM1 pin 24
set_property -dict {PACKAGE_PIN K17 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports tb_sor]      ;# JM1 pin 25
set_property -dict {PACKAGE_PIN M17 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports tb_strobe]   ;# JM1 pin 26
set_property -dict {PACKAGE_PIN K18 IOSTANDARD LVCMOS33 DRIVE 8 SLEW SLOW} [get_ports tb_busy]     ;# JM1 pin 27

## ---- GPU clock relationships
##   MMCM CLKOUT0 = core 148.75 MHz (also ACLK of M_AXI_GPU and S_AXI_HP0/1/2), CLKOUT1 = serial
##   371.875 MHz, CLKOUT2 = pixel 74.375 MHz; derived automatically from clk50. Core <-> pixel/serial
##   are asynchronous by design (async FIFO and 2-FF/toggle synchronisers only); pixel and serial stay
##   in one group (OSERDES CLK/CLKDIV). TIMING-47 (clock group between clocks of one VCO) is waived,
##   as in the standalone GPU build. The core <-> FCLK0 crossing is the GP0 interconnect's clock
##   converter, constrained by that IP's own XDC.
set_clock_groups -name core_vs_video -asynchronous \
    -group [get_clocks -of_objects [get_pins u_gpu/u_clkgen/u_mmcm/CLKOUT0]] \
    -group [get_clocks -of_objects [get_pins {u_gpu/u_clkgen/u_mmcm/CLKOUT1 u_gpu/u_clkgen/u_mmcm/CLKOUT2}]]

create_waiver -type METHODOLOGY -id {TIMING-47} -user "fpga-gpu" \
    -desc "core/pixel clock group is intentional: all CDC goes through the async FIFO or 2-FF synchronisers" \
    -objects [get_clocks -of_objects [get_pins u_gpu/u_clkgen/u_mmcm/CLKOUT0]] \
    -objects [get_clocks -of_objects [get_pins u_gpu/u_clkgen/u_mmcm/CLKOUT2]] -strings {"Clock Group"}
create_waiver -type METHODOLOGY -id {TIMING-47} -user "fpga-gpu" \
    -desc "core/pixel clock group is intentional: all CDC goes through the async FIFO or 2-FF synchronisers" \
    -objects [get_clocks -of_objects [get_pins u_gpu/u_clkgen/u_mmcm/CLKOUT2]] \
    -objects [get_clocks -of_objects [get_pins u_gpu/u_clkgen/u_mmcm/CLKOUT0]] -strings {"Clock Group"}

## ---- Scanout async FIFO gray pointers (accel/gpu/rtl/async_fifo.v): the clock group above removes
## core <-> pixel timing; set_bus_skew (not overridden by clock groups) keeps each 11-bit gray
## pointer's bits within 6.0 ns (< one source clock, < the destination period).
set_bus_skew -from [get_cells {u_gpu/u_scan/u_fifo/wgray_reg[*]}] -to [get_cells {u_gpu/u_scan/u_fifo/wgray_s1_reg[*]}] 6.000
set_bus_skew -from [get_cells {u_gpu/u_scan/u_fifo/rgray_reg[*]}] -to [get_cells {u_gpu/u_scan/u_fifo/rgray_s1_reg[*]}] 6.000

## ---- GPU asynchronous I/O: inputs go through 2-FF ASYNC_REG synchronisers, outputs are static/slow
set_false_path -from [get_ports {tb_d[*] tb_sor tb_strobe hdmi_hpd}]
set_false_path -to   [get_ports {tb_busy hdmi_out_en}]
## TMDS pads are driven straight by OSERDESE2 OQ -> OBUFDS: no register-to-pad path a
## set_output_delay could describe (the four lanes use identical OSERDES/OBUFDS paths in bank 34)
set_false_path -to   [get_ports {hdmi_d0_p hdmi_d0_n hdmi_d1_p hdmi_d1_n hdmi_d2_p hdmi_d2_n hdmi_clk_p hdmi_clk_n}]
