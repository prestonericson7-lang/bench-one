## pz7020_gpu.xdc -- PL constraints for the FPGA-GPU on the Puzhi PZ7020 (xc7z020clg400-2).
## Pin map = SPEC.md section 2 (balls from the Puzhi manual / pin xlsx). PS7 DDR and MIO pins are
## constrained by the processing_system7 IP itself and are not listed here.
## All PL I/O used here is in banks 34/35 at VCCO 3.3 V.

## ---------------------------------------------------------------------------------------------
## Clock input: 50 MHz oscillator
## ---------------------------------------------------------------------------------------------
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports clk50]
create_clock -period 20.000 -name clk50 [get_ports clk50]

## ---------------------------------------------------------------------------------------------
## LEDs
## ---------------------------------------------------------------------------------------------
set_property -dict {PACKAGE_PIN R19 IOSTANDARD LVCMOS33} [get_ports {led[0]}]
set_property -dict {PACKAGE_PIN V13 IOSTANDARD LVCMOS33} [get_ports {led[1]}]

## ---------------------------------------------------------------------------------------------
## HDMI (DVI signalling). TMDS_33 needs the sink's 50 ohm pull-ups to 3.3 V.
## Pairs: V17/V18, W18/W19, N17/P18, T17/R18 (P/N of the same IO_Lxx pair in bank 34).
## ---------------------------------------------------------------------------------------------
set_property -dict {PACKAGE_PIN V17 IOSTANDARD TMDS_33} [get_ports hdmi_d0_p]
set_property -dict {PACKAGE_PIN V18 IOSTANDARD TMDS_33} [get_ports hdmi_d0_n]
set_property -dict {PACKAGE_PIN W18 IOSTANDARD TMDS_33} [get_ports hdmi_d1_p]
set_property -dict {PACKAGE_PIN W19 IOSTANDARD TMDS_33} [get_ports hdmi_d1_n]
set_property -dict {PACKAGE_PIN N17 IOSTANDARD TMDS_33} [get_ports hdmi_d2_p]
set_property -dict {PACKAGE_PIN P18 IOSTANDARD TMDS_33} [get_ports hdmi_d2_n]
set_property -dict {PACKAGE_PIN T17 IOSTANDARD TMDS_33} [get_ports hdmi_clk_p]
set_property -dict {PACKAGE_PIN R18 IOSTANDARD TMDS_33} [get_ports hdmi_clk_n]

set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS33} [get_ports hdmi_out_en]
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS33} [get_ports hdmi_hpd]

## ---------------------------------------------------------------------------------------------
## Teensy 4.1 parallel bus on JM1 (SPEC section 3). tb_d[i] = JM1 pin 9+i.
## Inputs get a weak PULLDOWN: the Teensy keeps its pins high-Z until it has seen BUSY = 0 for
## 10 ms, so without a pull the floating STROBE could produce spurious transfers. A pull-down
## (not pull-up) never back-powers an unpowered Teensy.
## ---------------------------------------------------------------------------------------------
set_property -dict {PACKAGE_PIN E18 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[0]}]
set_property -dict {PACKAGE_PIN F16 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[1]}]
set_property -dict {PACKAGE_PIN E19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[2]}]
set_property -dict {PACKAGE_PIN F17 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[3]}]
set_property -dict {PACKAGE_PIN G17 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[4]}]
set_property -dict {PACKAGE_PIN B19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[5]}]
set_property -dict {PACKAGE_PIN G18 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[6]}]
set_property -dict {PACKAGE_PIN A20 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[7]}]
set_property -dict {PACKAGE_PIN D19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[8]}]
set_property -dict {PACKAGE_PIN C20 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[9]}]
set_property -dict {PACKAGE_PIN D20 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[10]}]
set_property -dict {PACKAGE_PIN B20 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[11]}]
set_property -dict {PACKAGE_PIN J18 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[12]}]
set_property -dict {PACKAGE_PIN K19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[13]}]
set_property -dict {PACKAGE_PIN H18 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[14]}]
set_property -dict {PACKAGE_PIN J19 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports {tb_d[15]}]
set_property -dict {PACKAGE_PIN K17 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports tb_sor]
set_property -dict {PACKAGE_PIN M17 IOSTANDARD LVCMOS33 PULLTYPE PULLDOWN} [get_ports tb_strobe]
set_property -dict {PACKAGE_PIN K18 IOSTANDARD LVCMOS33 DRIVE 8 SLEW SLOW} [get_ports tb_busy]

## ---------------------------------------------------------------------------------------------
## Clock relationships
##   CLKOUT0 = core 148.75 MHz (6.723 ns), CLKOUT1 = serial 371.875 MHz, CLKOUT2 = pixel
##   74.375 MHz (13.445 ns). The MMCM output clocks are derived automatically from clk50.
##   Core <-> pixel/serial are treated as asynchronous (CDC only through the scanout async FIFO
##   and 2-FF/toggle synchronisers); pixel and serial stay in one group (OSERDES CLK/CLKDIV are
##   timed against each other). Core and pixel come from the same VCO (148.75 = 2 x 74.375), so
##   report_methodology flags this clock group as TIMING-47 ("clock group between synchronous
##   clocks", 2 checks). That is intentional: every core <-> pixel crossing is synchronised, the
##   design does not rely on the phase relationship, and the core divide may change. The two
##   checks are waived below (waiver syntax taken from write_waivers in Vivado 2026.1).
## ---------------------------------------------------------------------------------------------
set_clock_groups -name core_vs_video -asynchronous \
    -group [get_clocks -of_objects [get_pins u_pl/u_clkgen/u_mmcm/CLKOUT0]] \
    -group [get_clocks -of_objects [get_pins {u_pl/u_clkgen/u_mmcm/CLKOUT1 u_pl/u_clkgen/u_mmcm/CLKOUT2}]]

create_waiver -type METHODOLOGY -id {TIMING-47} -user "fpga-gpu" \
    -desc "core/pixel clock group is intentional: all CDC goes through the async FIFO or 2-FF synchronisers" \
    -objects [get_clocks -of_objects [get_pins u_pl/u_clkgen/u_mmcm/CLKOUT0]] \
    -objects [get_clocks -of_objects [get_pins u_pl/u_clkgen/u_mmcm/CLKOUT2]] -strings {"Clock Group"}
create_waiver -type METHODOLOGY -id {TIMING-47} -user "fpga-gpu" \
    -desc "core/pixel clock group is intentional: all CDC goes through the async FIFO or 2-FF synchronisers" \
    -objects [get_clocks -of_objects [get_pins u_pl/u_clkgen/u_mmcm/CLKOUT2]] \
    -objects [get_clocks -of_objects [get_pins u_pl/u_clkgen/u_mmcm/CLKOUT0]] -strings {"Clock Group"}

## ---------------------------------------------------------------------------------------------
## Async FIFO gray pointers (scanout: rtl/async_fifo.v, review R1-03). The clock group above
## removes all core <-> pixel timing, which would leave the skew between the bits of each 11-bit
## gray-coded pointer unbounded. set_bus_skew is not overridden by set_clock_groups; it bounds
## the arrival spread of the bus bits at the ASYNC_REG synchroniser so the other domain samples
## at most one gray step in flight. Requirement 6.0 ns < the pointer update interval (one source
## clock, >= 6.723 ns) and < the destination period. Checked by report_bus_skew in build.tcl.
## ---------------------------------------------------------------------------------------------
set_bus_skew -from [get_cells {u_pl/u_scan/u_fifo/wgray_reg[*]}] -to [get_cells {u_pl/u_scan/u_fifo/wgray_s1_reg[*]}] 6.000
set_bus_skew -from [get_cells {u_pl/u_scan/u_fifo/rgray_reg[*]}] -to [get_cells {u_pl/u_scan/u_fifo/rgray_s1_reg[*]}] 6.000

## ---------------------------------------------------------------------------------------------
## Asynchronous I/O: inputs go through 2-FF ASYNC_REG synchronisers, outputs are static/slow
## ---------------------------------------------------------------------------------------------
set_false_path -from [get_ports {tb_d[*] tb_sor tb_strobe hdmi_hpd}]
set_false_path -to   [get_ports {led[*] tb_busy hdmi_out_en}]

## HDMI TMDS pads: driven straight by OSERDESE2 OQ -> OBUFDS, so there is no register-to-pad path
## a set_output_delay could describe; the four lanes use identical OSERDES/OBUFDS paths in bank 34
## (lane-to-lane skew = placement/package only). Without this, check_timing reports the 4 _p
## ports as "no output delay" (HIGH) in a real Vivado 2026.1 run.
set_false_path -to   [get_ports {hdmi_d0_p hdmi_d0_n hdmi_d1_p hdmi_d1_n hdmi_d2_p hdmi_d2_n hdmi_clk_p hdmi_clk_n}]

## ---------------------------------------------------------------------------------------------
## Configuration / bitstream
## ---------------------------------------------------------------------------------------------
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
