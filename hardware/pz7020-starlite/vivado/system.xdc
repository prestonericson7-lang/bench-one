# system.xdc -- PL pins of the Vivado system design (build_system.tcl) on the PZ7020-StarLite.
# Balls: manual Parts 3.5 (PL RGMII), 3.16 (LEDs), 3.17 (KEYs); JM1 from the CON pins xlsx.
# The PS side (DDR, MIO) is fixed silicon and needs no constraints.
set_property CFGBVS VCCO        [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]

## ---- register-file board I/O (pl_regs) ----
set_property -dict {PACKAGE_PIN R19 IOSTANDARD LVCMOS33} [get_ports led1]       ;# LED1 IO_0_34
set_property -dict {PACKAGE_PIN V13 IOSTANDARD LVCMOS33} [get_ports led2]       ;# LED2 IO_L3N_34
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
