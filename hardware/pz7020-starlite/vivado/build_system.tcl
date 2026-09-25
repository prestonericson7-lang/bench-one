# build_system.tcl -- the PZ7020-StarLite system design, AMD flow end to end:
#   PS7 (board config) + pl_regs on M_AXI_GP0 @0x4000_0000 + GEM1 over EMIO -> GMII-to-RGMII -> PL PHY
#   -> synth -> impl -> bitstream -> XSA (with bitstream) for the Vitis FSBL.
#
#   D:\2026.1\Vivado\bin\vivado.bat -mode batch -source build_system.tcl
#
# DDR: 32-bit bus = 1 GB (two MT41K256M16, per the vendor spec for the PZ7020 and the owner).
# Everything else of the PS starts from the parameter set of a working PetaLinux build for this
# board (ps7_validated.tcl), applied with catch, then our overrides on top.
set here [file dirname [file normalize [info script]]]
set root [file normalize $here/..]
set out  $here/build
file mkdir $out
proc say {m} { puts "SYS: $m" }

create_project system $here/prj -part xc7z020clg400-2 -force
set_property target_language Verilog [current_project]
add_files -norecurse $root/ps7-axi/pl_regs.v
add_files -fileset constrs_1 -norecurse $here/system.xdc
update_compile_order -fileset sources_1
create_bd_design system

# ---------------- PS7 ----------------
set ps7 [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 ps7]
source $here/ps7_validated.tcl
# two passes: some parameters (e.g. ENET0_ENET0_IO) only stick once their peripheral is enabled,
# and the JSON order is alphabetical, not dependency order
foreach pass {1 2} {
    set ok 0; set rej {}
    foreach {k v} $ps7_validated {
        if {[catch {set_property CONFIG.$k $v $ps7} e]} { lappend rej $k } else { incr ok }
    }
}
say "validated PS7 params applied: $ok, rejected (derived/read-only): [llength $rej] $rej"
set_property -dict [list \
    CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH      {32 Bit} \
    CONFIG.PCW_UIPARAM_DDR_MEMORY_TYPE    {DDR 3 (Low Voltage)} \
    CONFIG.PCW_UIPARAM_DDR_PARTNO         {MT41K256M16 RE-125} \
    CONFIG.PCW_UART0_BAUD_RATE            {115200} \
    CONFIG.PCW_ENET0_PERIPHERAL_ENABLE    {1} \
    CONFIG.PCW_ENET0_ENET0_IO             {MIO 16 .. 27} \
    CONFIG.PCW_ENET0_GRP_MDIO_ENABLE      {1} \
    CONFIG.PCW_ENET0_GRP_MDIO_IO          {MIO 52 .. 53} \
    CONFIG.PCW_ENET1_PERIPHERAL_ENABLE    {1} \
    CONFIG.PCW_ENET1_ENET1_IO             {EMIO} \
    CONFIG.PCW_ENET1_GRP_MDIO_ENABLE      {1} \
    CONFIG.PCW_ENET1_GRP_MDIO_IO          {EMIO} \
    CONFIG.PCW_ENET1_PERIPHERAL_FREQMHZ   {1000 Mbps} \
    CONFIG.PCW_USB0_RESET_ENABLE          {1} \
    CONFIG.PCW_USB0_RESET_IO              {MIO 46} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ   {100} \
    CONFIG.PCW_EN_CLK1_PORT               {1} \
    CONFIG.PCW_FPGA1_PERIPHERAL_FREQMHZ   {200} \
    CONFIG.PCW_USE_M_AXI_GP0              {1} \
] $ps7
foreach k {PCW_UIPARAM_DDR_BUS_WIDTH PCW_UIPARAM_DDR_MEMORY_TYPE PCW_UIPARAM_DDR_FREQ_MHZ PCW_DDR_RAM_HIGHADDR \
           PCW_APU_PERIPHERAL_FREQMHZ PCW_ACT_APU_PERIPHERAL_FREQMHZ PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ \
           PCW_ACT_FPGA1_PERIPHERAL_FREQMHZ PCW_ENET0_ENET0_IO PCW_ENET1_ENET1_IO PCW_UART0_UART0_IO \
           PCW_SD0_SD0_IO PCW_USB0_USB0_IO PCW_USB0_RESET_IO PCW_QSPI_QSPI_IO} {
    say "ps7 $k = [get_property CONFIG.$k $ps7]"
}
# hard check: every PS peripheral on the MIO pins the manual documents
foreach {k want} {PCW_ENET0_ENET0_IO {MIO 16 .. 27} PCW_ENET0_GRP_MDIO_IO {MIO 52 .. 53} PCW_UART0_UART0_IO {MIO 10 .. 11}
                  PCW_SD0_SD0_IO {MIO 40 .. 45} PCW_USB0_USB0_IO {MIO 28 .. 39} PCW_USB0_RESET_IO {MIO 46}
                  PCW_QSPI_QSPI_IO {MIO 1 .. 6} PCW_ENET1_ENET1_IO {EMIO} PCW_UIPARAM_DDR_BUS_WIDTH {32 Bit}
                  PCW_PRESET_BANK0_VOLTAGE {LVCMOS 3.3V} PCW_PRESET_BANK1_VOLTAGE {LVCMOS 1.8V}} {
    set got [get_property CONFIG.$k $ps7]
    if {$got ne $want} { say "PS7 CHECK FAIL: $k = '$got', want '$want'"; exit 3 }
}
say "PS7 check: all MIO assignments, bank voltages and the 32-bit DDR match the board"
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "0" Master "Disable" Slave "Disable"} $ps7

# ---------------- pl_regs on GP0 ----------------
set regs [create_bd_cell -type module -reference pl_regs regs]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config {Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} Master {/ps7/M_AXI_GP0} Slave {/regs/s_axi} ddr_seg {Auto} intc_ip {New AXI Interconnect} master_apm {0}} \
    [get_bd_intf_pins regs/s_axi]
foreach p {led1 led2 fan_pwm} {
    create_bd_port -dir O $p; connect_bd_net [get_bd_pins regs/$p] [get_bd_ports $p]
}
foreach p {key1_n key2_n fan_tach} {
    create_bd_port -dir I $p; connect_bd_net [get_bd_ports $p] [get_bd_pins regs/$p]
}
set seg [get_bd_addr_segs -of [get_bd_addr_spaces ps7/Data] -filter {NAME =~ *regs*}]
if {$seg eq ""} { assign_bd_address; set seg [get_bd_addr_segs -of [get_bd_addr_spaces ps7/Data] -filter {NAME =~ *regs*}] }
set_property offset 0x40000000 $seg
set_property range 4K $seg
say "pl_regs address segment: $seg offset [get_property offset $seg] range [get_property range $seg]"

# ---------------- GEM1 -> GMII-to-RGMII -> PL PHY ----------------
set g [create_bd_cell -type ip -vlnv xilinx.com:ip:gmii_to_rgmii g2r]
set_property CONFIG.SupportLevel {Include_Shared_Logic_in_Core} $g
set_property CONFIG.C_PHYADDR {8} $g
# RTL8211F datasheet: TXDLY strap has an internal PULL-DOWN (no TX delay unless strapped up) and the
# schematic leaves the delay straps NC -> the MAC side must skew TXC. RXDLY has an internal pull-UP
# (PHY adds the RX delay), so RX needs no skew from us.
if {[catch {set_property CONFIG.RGMII_TXC_SKEW {2} $g} e]} { say "RGMII_TXC_SKEW=2 rejected: $e" }
say "g2r: SupportLevel=[get_property CONFIG.SupportLevel $g] PHYADDR=[get_property CONFIG.C_PHYADDR $g] TXC_SKEW=[get_property CONFIG.RGMII_TXC_SKEW $g]"
connect_bd_intf_net [get_bd_intf_pins ps7/GMII_ETHERNET_1] [get_bd_intf_pins g2r/GMII]
connect_bd_intf_net [get_bd_intf_pins ps7/MDIO_ETHERNET_1] [get_bd_intf_pins g2r/MDIO_GEM]
# explicit ports (make_bd_intf_pins_external returns nothing in 2026.1, and fixed names keep system.xdc valid)
foreach {pin port} {RGMII rgmii MDIO_PHY mdio_phy} {
    set ip [get_bd_intf_pins g2r/$pin]
    create_bd_intf_port -mode [get_property MODE $ip] -vlnv [get_property VLNV $ip] $port
    connect_bd_intf_net $ip [get_bd_intf_ports $port]
}
connect_bd_net [get_bd_pins ps7/FCLK_CLK1] [get_bd_pins g2r/clkin]
set rst [lindex [get_bd_cells -filter {VLNV =~ *proc_sys_reset*}] 0]
foreach p {tx_reset rx_reset} {
    if {[llength [get_bd_pins g2r/$p]]} { connect_bd_net [get_bd_pins $rst/peripheral_reset] [get_bd_pins g2r/$p] }
}
say "g2r pins: [get_bd_pins -of $g]"
foreach p [get_bd_pins -of $g] { if {[llength [get_bd_nets -of $p]] == 0} { say "g2r UNCONNECTED pin $p dir=[get_property DIR $p]" } }

assign_bd_address
validate_bd_design
save_bd_design
set bdf [get_files system.bd]
generate_target all $bdf
set wrapper [make_wrapper -files $bdf -top]
add_files -norecurse $wrapper
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1

# ---------------- implement ----------------
launch_runs synth_1 -jobs 8
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] ne "100%"} { say "SYNTH FAILED"; exit 1 }
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} { say "IMPL FAILED"; exit 1 }
open_run impl_1
report_utilization    -file $out/system_util.rpt
report_timing_summary -file $out/system_timing.rpt
report_drc            -file $out/system_drc.rpt
set wns [get_property SLACK [get_timing_paths -max_paths 1 -setup]]
set whs [get_property SLACK [get_timing_paths -max_paths 1 -hold]]
say "timing: WNS=$wns ns WHS=$whs ns"
foreach c {"Slice LUTs" "Slice Registers" "Block RAM Tile" "DSPs" "Bonded IOB" "BUFGCTRL" "MMCME2_ADV" "IDELAYE2"} {
    set u [report_utilization -return_string]
    if {[regexp "\\|\\s*${c}\\*?\\s*\\|\\s*(\[0-9.\]+)\\s*\\|\[^|\]*\\|\[^|\]*\\|\\s*(\[0-9.\]+)" $u -> used avail]} { say "util $c: $used / $avail" }
}
set bit [glob $here/prj/system.runs/impl_1/*.bit]
file copy -force $bit $out/system.bit
write_hw_platform -fixed -include_bit -force -file $out/system.xsa
say "bitstream: $out/system.bit [file size $out/system.bit] B"
say "xsa: $out/system.xsa [file size $out/system.xsa] B"
say "DONE"
