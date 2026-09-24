## ===========================================================================================
##  pz7020_starlite_board.xdc -- MASTER board constraints for the Puzhi PZ7020-StarLite
##  Part: xc7z020clg400-2   (manual states XC7Z020-2CLG400I; confirm the marking on your chip)
##
##  Every ball below is transcribed from the vendor documents in the bundle:
##    [M x.y]  = "PZ7010-StarLite/PZ7020-StarLite User Manual" V1.0, Part x.y
##    [XLSX]   = "Puzhi PZ-Starlite CON Pins Signal and Equal Length.xlsx"  (authoritative for JM1/JM2)
##    [SCH n]  = "Puzhi PZ-StarLite Schematic.pdf" V1.0, sheet n
##  Nothing here is inferred. If a line has no tag, it comes from the same table as the line above it.
##
##  HOW TO USE: copy the blocks your top-level actually has ports for, rename the ports, and
##  UNCOMMENT them. Vivado errors on a constraint for a port that does not exist, so everything
##  except the two config properties ships commented out.
## ===========================================================================================

## ---- configuration bank voltage (BANK0 VCCO = 3.3 V on this board) [SCH 4: VDD_3V3 on bank 0]
set_property CFGBVS VCCO        [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]

## ===========================================================================================
##  CLOCK  -- 50 MHz single-ended oscillator Y2 on IO_12P_MRCC_34, ball U18   [M 3.2] [SCH 4]
##  (PS reference clock is a separate 33.333 MHz oscillator Y1 on PS_CLK, ball E7 -- PS side,
##   not a PL constraint.)
## ===========================================================================================
#set_property PACKAGE_PIN U18      [get_ports clk_50m]   ;# PL_CLK_50M  IO_12P_MRCC_34
#set_property IOSTANDARD  LVCMOS33 [get_ports clk_50m]
#create_clock -period 20.000 -name clk_50m -waveform {0.000 10.000} [get_ports clk_50m]

## ===========================================================================================
##  RESET  -- nGRST key, active LOW, SHARED: PS_POR_B (C7) and PL IO_L12N_MRCC_34 (U19)  [M 3.3]
##  Pressing it power-on-resets the PS as well as pulsing the PL pin. Treat as async in RTL.
## ===========================================================================================
#set_property PACKAGE_PIN U19      [get_ports rst_n]     ;# PL_nGRST    IO_L12N_MRCC_34
#set_property IOSTANDARD  LVCMOS33 [get_ports rst_n]
#set_false_path -from [get_ports rst_n]

## ===========================================================================================
##  USER LEDs (BANK34, HIGH = lit)  [M 3.16]      USER KEYs (BANK35, LOW = pressed)  [M 3.17]
## ===========================================================================================
#set_property PACKAGE_PIN R19      [get_ports led1]     ;# LED1  IO_0_34
#set_property IOSTANDARD  LVCMOS33 [get_ports led1]
#set_property PACKAGE_PIN V13      [get_ports led2]     ;# LED2  IO_L3N_34
#set_property IOSTANDARD  LVCMOS33 [get_ports led2]

#set_property PACKAGE_PIN G14      [get_ports key1_n]   ;# KEY1  IO_0_35
#set_property IOSTANDARD  LVCMOS33 [get_ports key1_n]
#set_property PULLUP      true     [get_ports key1_n]
#set_property PACKAGE_PIN J15      [get_ports key2_n]   ;# KEY2  IO_25_35
#set_property IOSTANDARD  LVCMOS33 [get_ports key2_n]
#set_property PULLUP      true     [get_ports key2_n]

## ===========================================================================================
##  JM1 -- 40-pin 2.54 mm header, ALL BANK35 (HR bank, 1.8/2.5/3.3 V by resistor, DEFAULT 3.3 V)
##  [XLSX sheet JM1] [M 3.18] [M 3.7]
##  Pin 1 = 5 V (== VDD_5V rail, hard-tied to the JTAG Type-C VBUS -- never two 5 V sources),
##  pin 2 = 3.3 V regulator OUTPUT, pins 3/4/33/34/35/36 = GND. The pairs are true LVDS-capable
##  differential pairs (P/N of one IO_Lxx). Trace lengths are matched to ~1770 mil (XLSX).
##  Port names below are placeholders: jm1_<pin>.
## ===========================================================================================
#set_property PACKAGE_PIN H16 [get_ports jm1_5 ]   ;# IO_13P_35
#set_property PACKAGE_PIN E17 [get_ports jm1_6 ]   ;# IO_3P_35
#set_property PACKAGE_PIN H17 [get_ports jm1_7 ]   ;# IO_13N_35
#set_property PACKAGE_PIN D18 [get_ports jm1_8 ]   ;# IO_3N_35
#set_property PACKAGE_PIN E18 [get_ports jm1_9 ]   ;# IO_5P_35
#set_property PACKAGE_PIN F16 [get_ports jm1_10]   ;# IO_6P_35
#set_property PACKAGE_PIN E19 [get_ports jm1_11]   ;# IO_5N_35
#set_property PACKAGE_PIN F17 [get_ports jm1_12]   ;# IO_6N_35
#set_property PACKAGE_PIN G17 [get_ports jm1_13]   ;# IO_16P_35
#set_property PACKAGE_PIN B19 [get_ports jm1_14]   ;# IO_2P_35
#set_property PACKAGE_PIN G18 [get_ports jm1_15]   ;# IO_16N_35
#set_property PACKAGE_PIN A20 [get_ports jm1_16]   ;# IO_2N_35
#set_property PACKAGE_PIN D19 [get_ports jm1_17]   ;# IO_4P_35
#set_property PACKAGE_PIN C20 [get_ports jm1_18]   ;# IO_1P_35
#set_property PACKAGE_PIN D20 [get_ports jm1_19]   ;# IO_4N_35
#set_property PACKAGE_PIN B20 [get_ports jm1_20]   ;# IO_1N_35
#set_property PACKAGE_PIN J18 [get_ports jm1_21]   ;# IO_14P_35
#set_property PACKAGE_PIN K19 [get_ports jm1_22]   ;# IO_10P_35
#set_property PACKAGE_PIN H18 [get_ports jm1_23]   ;# IO_14N_35
#set_property PACKAGE_PIN J19 [get_ports jm1_24]   ;# IO_10N_35
#set_property PACKAGE_PIN K17 [get_ports jm1_25]   ;# IO_12P_35
#set_property PACKAGE_PIN M17 [get_ports jm1_26]   ;# IO_8P_35
#set_property PACKAGE_PIN K18 [get_ports jm1_27]   ;# IO_12N_35
#set_property PACKAGE_PIN M18 [get_ports jm1_28]   ;# IO_8N_35
#set_property PACKAGE_PIN L16 [get_ports jm1_29]   ;# IO_11P_35
#set_property PACKAGE_PIN F19 [get_ports jm1_30]   ;# IO_15P_35
#set_property PACKAGE_PIN L17 [get_ports jm1_31]   ;# IO_11N_35
#set_property PACKAGE_PIN F20 [get_ports jm1_32]   ;# IO_15N_35
#set_property PACKAGE_PIN M19 [get_ports jm1_37]   ;# IO_7P_35
#set_property PACKAGE_PIN L19 [get_ports jm1_38]   ;# IO_9P_35
#set_property PACKAGE_PIN M20 [get_ports jm1_39]   ;# IO_7N_35
#set_property PACKAGE_PIN L20 [get_ports jm1_40]   ;# IO_9N_35
#set_property IOSTANDARD LVCMOS33 [get_ports jm1_*]

## ===========================================================================================
##  JM2 -- pins 5-20 = BANK35, pins 21-40 = BANK34   [XLSX sheet JM2]
##  !! BANK34 also carries HDMI (TMDS_33), the LEDs, the PL reset and the PL Ethernet PHY, so its
##  VCCO is 3.3 V in practice. Changing the BANK35 level resistor does NOT change pins 21-40.
##  Trace lengths ~1690 mil (XLSX).
## ===========================================================================================
#set_property PACKAGE_PIN G19 [get_ports jm2_5 ]   ;# IO_18P_35
#set_property PACKAGE_PIN J20 [get_ports jm2_6 ]   ;# IO_17P_35
#set_property PACKAGE_PIN G20 [get_ports jm2_7 ]   ;# IO_18N_35
#set_property PACKAGE_PIN H20 [get_ports jm2_8 ]   ;# IO_17N_35
#set_property PACKAGE_PIN H15 [get_ports jm2_9 ]   ;# IO_19P_35
#set_property PACKAGE_PIN K14 [get_ports jm2_10]   ;# IO_20P_35
#set_property PACKAGE_PIN G15 [get_ports jm2_11]   ;# IO_19N_35
#set_property PACKAGE_PIN J14 [get_ports jm2_12]   ;# IO_20N_35
#set_property PACKAGE_PIN K16 [get_ports jm2_13]   ;# IO_24P_35
#set_property PACKAGE_PIN L14 [get_ports jm2_14]   ;# IO_22P_35
#set_property PACKAGE_PIN J16 [get_ports jm2_15]   ;# IO_24N_35
#set_property PACKAGE_PIN L15 [get_ports jm2_16]   ;# IO_22N_35
#set_property PACKAGE_PIN N15 [get_ports jm2_17]   ;# IO_21P_35
#set_property PACKAGE_PIN M14 [get_ports jm2_18]   ;# IO_23P_35
#set_property PACKAGE_PIN N16 [get_ports jm2_19]   ;# IO_21N_35
#set_property PACKAGE_PIN M15 [get_ports jm2_20]   ;# IO_23N_35
#set_property PACKAGE_PIN T16 [get_ports jm2_21]   ;# IO_9P_34   (BANK34 from here on)
#set_property PACKAGE_PIN T14 [get_ports jm2_22]   ;# IO_5P_34
#set_property PACKAGE_PIN U17 [get_ports jm2_23]   ;# IO_9N_34
#set_property PACKAGE_PIN T15 [get_ports jm2_24]   ;# IO_5N_34
#set_property PACKAGE_PIN P14 [get_ports jm2_25]   ;# IO_6P_34
#set_property PACKAGE_PIN T12 [get_ports jm2_26]   ;# IO_2P_34
#set_property PACKAGE_PIN R14 [get_ports jm2_27]   ;# IO_6N_34
#set_property PACKAGE_PIN U12 [get_ports jm2_28]   ;# IO_2N_34
#set_property PACKAGE_PIN T11 [get_ports jm2_29]   ;# IO_1P_34
#set_property PACKAGE_PIN Y16 [get_ports jm2_30]   ;# IO_7P_34
#set_property PACKAGE_PIN T10 [get_ports jm2_31]   ;# IO_1N_34
#set_property PACKAGE_PIN Y17 [get_ports jm2_32]   ;# IO_7N_34
#set_property PACKAGE_PIN V12 [get_ports jm2_37]   ;# IO_4P_34
#set_property PACKAGE_PIN W14 [get_ports jm2_38]   ;# IO_8P_34
#set_property PACKAGE_PIN W13 [get_ports jm2_39]   ;# IO_4N_34
#set_property PACKAGE_PIN Y14 [get_ports jm2_40]   ;# IO_8N_34
#set_property IOSTANDARD LVCMOS33 [get_ports jm2_*]

## ===========================================================================================
##  PL-SIDE GIGABIT ETHERNET -- RTL8211F-CG (U16), RGMII, PHY address 2 [SCH 16: PHY_AD[2:0]=010]
##  Pins on BANK34  [M 3.5].  The PS-side PHY (U15, address 1) is on MIO16-27 + MDIO MIO52/53 and
##  is NOT a PL constraint -- it is configured in the PS7 block (see PS-CONFIG.md).
##  RGMII TX/RX delay straps: R144/R146 are 0 R "(NC)" on the schematic, so the delay mode is
##  NOT known from the documents -- determine at bring-up (try rgmii-id first, then rgmii).
## ===========================================================================================
#set_property PACKAGE_PIN V20 [get_ports rgmii_txc]      ;# PL_GPHY_GTX_CLK  IO_L16P_34
#set_property PACKAGE_PIN N20 [get_ports {rgmii_td[0]}]  ;# PL_GPHY_TXD0     IO_L14P_34
#set_property PACKAGE_PIN P20 [get_ports {rgmii_td[1]}]  ;# PL_GPHY_TXD1     IO_L14N_34
#set_property PACKAGE_PIN T20 [get_ports {rgmii_td[2]}]  ;# PL_GPHY_TXD2     IO_L15P_34
#set_property PACKAGE_PIN U20 [get_ports {rgmii_td[3]}]  ;# PL_GPHY_TXD3     IO_L15N_34
#set_property PACKAGE_PIN W20 [get_ports rgmii_tx_ctl]   ;# PL_GPHY_TX_EN    IO_L16N_34
#set_property PACKAGE_PIN N18 [get_ports rgmii_rxc]      ;# PL_GPHY_RX_CLK   IO_L13P_34  (clock-capable)
#set_property PACKAGE_PIN Y18 [get_ports {rgmii_rd[0]}]  ;# PL_GPHY_RXD0     IO_L17P_34
#set_property PACKAGE_PIN Y19 [get_ports {rgmii_rd[1]}]  ;# PL_GPHY_RXD1     IO_L17N_34
#set_property PACKAGE_PIN V16 [get_ports {rgmii_rd[2]}]  ;# PL_GPHY_RXD2     IO_L18P_34
#set_property PACKAGE_PIN W16 [get_ports {rgmii_rd[3]}]  ;# PL_GPHY_RXD3     IO_L18N_34
#set_property PACKAGE_PIN P19 [get_ports rgmii_rx_ctl]   ;# PL_GPHY_RX_DV    IO_L13N_34
#set_property PACKAGE_PIN U14 [get_ports mdio_mdc]       ;# PL_GPHY_MDC      IO_L11P_34
#set_property PACKAGE_PIN U15 [get_ports mdio_mdio]      ;# PL_GPHY_MDIO     IO_L11N_34
#set_property IOSTANDARD LVCMOS33 [get_ports {rgmii_* mdio_*}]
#create_clock -period 8.000 -name rgmii_rxc [get_ports rgmii_rxc]   ;# 125 MHz from the PHY

## ===========================================================================================
##  EEPROM AT24C64D -- I2C on BANK34, 3.3 V, addresses 0xA0 write / 0xA1 read  [M 3.13]
## ===========================================================================================
#set_property PACKAGE_PIN V15 [get_ports eeprom_scl]     ;# IO_L10P_34
#set_property PACKAGE_PIN W15 [get_ports eeprom_sda]     ;# IO_L10N_34
#set_property IOSTANDARD LVCMOS33 [get_ports {eeprom_scl eeprom_sda}]
#set_property PULLUP true         [get_ports {eeprom_scl eeprom_sda}]

## ===========================================================================================
##  HDMI OUTPUT -- TMDS on BANK34  [M 3.15]
## ===========================================================================================
#set_property PACKAGE_PIN T17 [get_ports hdmi_clk_p]     ;# HDMI_CLK_P   IO_L20P_34
#set_property PACKAGE_PIN R18 [get_ports hdmi_clk_n]     ;# HDMI_CLK_N   IO_L20N_34
#set_property PACKAGE_PIN V17 [get_ports {hdmi_d_p[0]}]  ;# HDMI_DATA0_P IO_L21P_34
#set_property PACKAGE_PIN V18 [get_ports {hdmi_d_n[0]}]  ;# HDMI_DATA0_N IO_L21N_34
#set_property PACKAGE_PIN W18 [get_ports {hdmi_d_p[1]}]  ;# HDMI_DATA1_P IO_L22P_34
#set_property PACKAGE_PIN W19 [get_ports {hdmi_d_n[1]}]  ;# HDMI_DATA1_N IO_L22N_34
#set_property PACKAGE_PIN N17 [get_ports {hdmi_d_p[2]}]  ;# HDMI_DATA2_P IO_L23P_34
#set_property PACKAGE_PIN P18 [get_ports {hdmi_d_n[2]}]  ;# HDMI_DATA2_N IO_L23N_34
#set_property IOSTANDARD TMDS_33 [get_ports {hdmi_clk_* hdmi_d_*}]
#set_property PACKAGE_PIN R16 [get_ports hdmi_scl]       ;# HDMI_IIC_SCL IO_L19P_34
#set_property PACKAGE_PIN R17 [get_ports hdmi_sda]       ;# HDMI_IIC_SDA IO_L19N_34
#set_property PACKAGE_PIN T19 [get_ports hdmi_cec]       ;# HDMI_CEC     IO_25_34
#set_property PACKAGE_PIN P15 [get_ports hdmi_hpd]       ;# HDMI_HPD     IO_L24P_34
#set_property PACKAGE_PIN P16 [get_ports hdmi_out_en]    ;# HDMI_OUT_EN  IO_L24N_34
#set_property IOSTANDARD LVCMOS33 [get_ports {hdmi_scl hdmi_sda hdmi_cec hdmi_hpd hdmi_out_en}]

## ===========================================================================================
##  MIPI CSI-2 (2 lanes) on BANK13  [M 3.14]
##  BANK13's VCCO is not stated in the manual; D-PHY HS lanes need LVDS_25/HSUL and the LP
##  lines 1.2 V-class I/O -- resolve the bank voltage from the schematic before using these.
## ===========================================================================================
#set_property PACKAGE_PIN Y7  [get_ports mipi_clk_p]     ;# IO_L13P_13
#set_property PACKAGE_PIN Y6  [get_ports mipi_clk_n]     ;# IO_L13N_13
#set_property PACKAGE_PIN U7  [get_ports {mipi_d_p[0]}]  ;# IO_L11P_13
#set_property PACKAGE_PIN V7  [get_ports {mipi_d_n[0]}]  ;# IO_L11N_13
#set_property PACKAGE_PIN T9  [get_ports {mipi_d_p[1]}]  ;# IO_L12P_13
#set_property PACKAGE_PIN U10 [get_ports {mipi_d_n[1]}]  ;# IO_L12N_13
#set_property PACKAGE_PIN W10 [get_ports {mipi_lp_p[0]}] ;# IO_L16P_13
#set_property PACKAGE_PIN W9  [get_ports {mipi_lp_n[0]}] ;# IO_L16N_13
#set_property PACKAGE_PIN U9  [get_ports {mipi_lp_p[1]}] ;# IO_L17P_13
#set_property PACKAGE_PIN U8  [get_ports {mipi_lp_n[1]}] ;# IO_L17N_13
#set_property PACKAGE_PIN W11 [get_ports mipi_cam_rst_n] ;# IO_L18P_13
#set_property PACKAGE_PIN W8  [get_ports mipi_cam_clk]   ;# IO_L15N_13
#set_property PACKAGE_PIN Y11 [get_ports cam_scl]        ;# IO_L18N_13
#set_property PACKAGE_PIN T5  [get_ports cam_sda]        ;# IO_L19P_13
