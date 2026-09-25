# probe.tcl -- create the block-design cells once and print their real pin/interface names (2026.1),
# so build_system.tcl wires by name instead of by guess.
set here [file dirname [file normalize [info script]]]
set root [file normalize $here/..]
file delete -force $here/probe_prj
create_project probe $here/probe_prj -part xc7z020clg400-2 -force
add_files $root/ps7-axi/pl_regs.v
update_compile_order -fileset sources_1
create_bd_design probe

set ps7 [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 ps7]
set_property -dict [list CONFIG.PCW_ENET1_PERIPHERAL_ENABLE {1} CONFIG.PCW_ENET1_ENET1_IO {EMIO} \
    CONFIG.PCW_ENET1_GRP_MDIO_ENABLE {1} CONFIG.PCW_EN_CLK1_PORT {1} CONFIG.PCW_FPGA1_PERIPHERAL_FREQMHZ {200} \
    CONFIG.PCW_USE_M_AXI_GP0 {1}] $ps7
puts "PROBE ps7 vlnv: [get_property VLNV $ps7]"
puts "PROBE ps7 intf: [get_bd_intf_pins -of $ps7]"
puts "PROBE ps7 pins: [get_bd_pins -of $ps7]"

set g [create_bd_cell -type ip -vlnv xilinx.com:ip:gmii_to_rgmii g2r]
puts "PROBE g2r vlnv: [get_property VLNV $g]"
puts "PROBE g2r intf: [get_bd_intf_pins -of $g]"
puts "PROBE g2r pins: [get_bd_pins -of $g]"
foreach p [lsort [list_property $g CONFIG.*]] { puts "PROBE g2r cfg $p = [get_property $p $g]" }

set r [create_bd_cell -type module -reference pl_regs regs]
puts "PROBE regs intf: [get_bd_intf_pins -of $r]"
puts "PROBE regs pins: [get_bd_pins -of $r]"
foreach ip [get_bd_intf_pins -of $r] { puts "PROBE regs intf $ip mode=[get_property MODE $ip] vlnv=[get_property VLNV $ip]" }
foreach p [get_bd_pins -of $r -filter {TYPE==clk || TYPE==rst}] { puts "PROBE regs $p type=[get_property TYPE $p] assoc=[get_property CONFIG.ASSOCIATED_BUSIF $p]" }
close_project
