# build_system.tcl -- the ONE PL image of the PZ7020-StarLite, AMD flow end to end.
#   The Zynq holds one bitstream at a time, so this design carries everything the PL does:
#     platform  PS7 (board config, 1 GB 32-bit DDR3) + pl_regs @0x4000_0000 + GEM1 over EMIO ->
#               GMII-to-RGMII -> PL PHY (eth1)
#     GPU       accel/gpu/rtl/gpu_pl.v (own MMCM from the 50 MHz U18 clock, core 148.75 MHz, HDMI,
#               Teensy bus): registers @0x43C0_0000 (4K) via GP0, masters on S_AXI_HP0/1/2
#     matrix    AXI DMA (simple mode) @0x4040_0000 (64K) + accel/rtl/zaccel_gemv.v between
#               M_AXIS_MM2S and S_AXIS_S2MM, memory side on S_AXI_HP3; all on FCLK0 100 MHz;
#               the engine's reset = system reset AND both DMA channel resets (gemv_reset.v)
#   top system_top.v (block design wrapper + gpu_pl) -> synth -> impl -> bitstream -> XSA.
#
#   D:\2026.1\Vivado\bin\vivado.bat -mode batch -source build_system.tcl
#
# Exit: 0 = bitstream + XSA written, timing met on every clock, 1 = synth/impl failed,
#       2 = timing NOT met (no system.bit/system.xsa written), 3 = PS7 check failed,
#       4 = address map / HP port check failed, 5 = block design wrapper does not match system_top.v,
#       6 = a package ball is used twice.
#
# DDR: this PS7 block says 32-bit / 1 GB (from the listing) -- WRONG for the board, which has ONE x16
# MT41K256M16 (512 MB, 16-bit bus; schematic wires DQ0-15 and A0-A14 only). The SPL built from this
# design's ps7_init was silent on the board (2026-09-26), so linux/build_uboot.sh uses ps7/ (16-bit)
# and refuses ps7-vivado/. The PL bitstream does not depend on the DDR width.
# Everything else of the PS starts from the parameter set of a working PetaLinux build for this
# board (ps7_validated.tcl), applied with catch, then our overrides on top.
set here [file dirname [file normalize [info script]]]
set root [file normalize $here/..]                 ;# hardware/pz7020-starlite
set repo [file normalize $root/../..]              ;# repository root
set gpu_rtl [file normalize $repo/accel/gpu/rtl]
set gemv_v  [file normalize $repo/accel/rtl/zaccel_gemv.v]
set out  $here/build
file mkdir $out
proc say {m} { puts "SYS: $m"; flush stdout }
proc die {code m} { puts "SYS: FAIL: $m"; flush stdout; exit $code }
proc must {what script} { if {[catch {uplevel 1 $script} e]} { die 1 "$what: $e" } }

# stale outputs never survive a failed run
foreach f {system.bit system.xsa system_util.rpt system_util_hier.rpt system_timing.rpt system_drc.rpt
           system_methodology.rpt system_clocks.rpt system_clock_interaction.rpt system_bus_skew.rpt
           system_cdc.rpt system_summary.txt TIMING_FAILED.txt} {
    file delete -force $out/$f
}
file delete -force $out/ps7_init

create_project system $here/prj -part xc7z020clg400-2 -force
set_property target_language Verilog [current_project]
add_files -norecurse $root/ps7-axi/pl_regs.v
add_files -norecurse $gemv_v
add_files -norecurse $here/gemv_reset.v
set gemv_stamp "[file size $gemv_v] B, [clock format [file mtime $gemv_v] -format {%Y-%m-%d %H:%M:%S}]"
say "matrix engine RTL: $gemv_v ($gemv_stamp)"
# GPU: every rtl/*.v except the standalone board top (gpu_top.v wraps the GPU's own 16-bit ps7_bd);
# rtl/sim_stubs is a subdirectory and is not globbed
set gpu_files {}
foreach f [lsort [glob -directory $gpu_rtl -types f *.v]] {
    if {[file tail $f] ne "gpu_top.v"} { lappend gpu_files $f }
}
foreach req {gpu_pl.v clkgen.v par_rx.v sync_fifo.v axi_gp_regs.v core_top.v scanout.v} {
    if {[lsearch -glob $gpu_files */$req] < 0} { die 1 "missing $gpu_rtl/$req" }
}
add_files -norecurse $gpu_files
add_files -norecurse $gpu_rtl/gpu_defs.vh
set_property file_type {Verilog Header} [get_files gpu_defs.vh]
set_property include_dirs [list $gpu_rtl] [get_filesets sources_1]
add_files -norecurse $here/system_top.v
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
    CONFIG.PCW_EN_RST0_PORT               {1} \
    CONFIG.PCW_USE_M_AXI_GP0              {1} \
    CONFIG.PCW_USE_S_AXI_HP0              {1} \
    CONFIG.PCW_USE_S_AXI_HP1              {1} \
    CONFIG.PCW_USE_S_AXI_HP2              {1} \
    CONFIG.PCW_USE_S_AXI_HP3              {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH       {64} \
    CONFIG.PCW_S_AXI_HP1_DATA_WIDTH       {64} \
    CONFIG.PCW_S_AXI_HP2_DATA_WIDTH       {64} \
    CONFIG.PCW_S_AXI_HP3_DATA_WIDTH       {64} \
] $ps7
foreach k {PCW_UIPARAM_DDR_BUS_WIDTH PCW_UIPARAM_DDR_MEMORY_TYPE PCW_UIPARAM_DDR_FREQ_MHZ PCW_DDR_RAM_HIGHADDR \
           PCW_APU_PERIPHERAL_FREQMHZ PCW_ACT_APU_PERIPHERAL_FREQMHZ PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ \
           PCW_ACT_FPGA1_PERIPHERAL_FREQMHZ PCW_ENET0_ENET0_IO PCW_ENET1_ENET1_IO PCW_UART0_UART0_IO \
           PCW_SD0_SD0_IO PCW_USB0_USB0_IO PCW_USB0_RESET_IO PCW_QSPI_QSPI_IO \
           PCW_USE_S_AXI_HP0 PCW_USE_S_AXI_HP1 PCW_USE_S_AXI_HP2 PCW_USE_S_AXI_HP3 \
           PCW_S_AXI_HP0_DATA_WIDTH PCW_S_AXI_HP1_DATA_WIDTH PCW_S_AXI_HP2_DATA_WIDTH PCW_S_AXI_HP3_DATA_WIDTH} {
    say "ps7 $k = [get_property CONFIG.$k $ps7]"
}
# hard check: every PS peripheral on the MIO pins the manual documents, the 32-bit DDR, and the four
# 64-bit HP ports the GPU (HP0-2) and the DMA (HP3) use
foreach {k want} {PCW_ENET0_ENET0_IO {MIO 16 .. 27} PCW_ENET0_GRP_MDIO_IO {MIO 52 .. 53} PCW_UART0_UART0_IO {MIO 10 .. 11}
                  PCW_SD0_SD0_IO {MIO 40 .. 45} PCW_USB0_USB0_IO {MIO 28 .. 39} PCW_USB0_RESET_IO {MIO 46}
                  PCW_QSPI_QSPI_IO {MIO 1 .. 6} PCW_ENET1_ENET1_IO {EMIO} PCW_UIPARAM_DDR_BUS_WIDTH {32 Bit}
                  PCW_PRESET_BANK0_VOLTAGE {LVCMOS 3.3V} PCW_PRESET_BANK1_VOLTAGE {LVCMOS 1.8V}
                  PCW_USE_M_AXI_GP0 1
                  PCW_USE_S_AXI_HP0 1 PCW_USE_S_AXI_HP1 1 PCW_USE_S_AXI_HP2 1 PCW_USE_S_AXI_HP3 1
                  PCW_S_AXI_HP0_DATA_WIDTH 64 PCW_S_AXI_HP1_DATA_WIDTH 64
                  PCW_S_AXI_HP2_DATA_WIDTH 64 PCW_S_AXI_HP3_DATA_WIDTH 64} {
    set got [get_property CONFIG.$k $ps7]
    if {$got ne $want} { die 3 "PS7 CHECK: $k = '$got', want '$want'" }
}
set f0 [get_property CONFIG.PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ $ps7]
if {abs($f0 - 100.0) > 0.001} { die 3 "PS7 CHECK: FCLK0 is $f0 MHz, the matrix engine contract is 100 MHz" }
say "PS7 check: MIO assignments, bank voltages, 32-bit DDR, FCLK0 100 MHz, HP0-3 enabled at 64 bit"
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "0" Master "Disable" Slave "Disable"} $ps7

# ---------------- clocks and resets ----------------
# FCLK0 (100 MHz): GP0, pl_regs, DMA, matrix engine, HP3.  gpu_aclk (148.75 MHz, from the GPU's
# MMCM in system_top): the GPU leg of the GP0 interconnect and S_AXI_HP0/1/2.
set fclk0 [get_bd_pins ps7/FCLK_CLK0]
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_ps7_100M]
connect_bd_net $fclk0 [get_bd_pins rst_ps7_100M/slowest_sync_clk]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rst_ps7_100M/ext_reset_in]
set pa [get_bd_pins rst_ps7_100M/peripheral_aresetn]
set ia [get_bd_pins rst_ps7_100M/interconnect_aresetn]

set gclk [create_bd_port -dir I -type clk -freq_hz 148750000 gpu_aclk]
set rstg [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_gpu]
connect_bd_net $gclk [get_bd_pins rst_gpu/slowest_sync_clk]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rst_gpu/ext_reset_in]

# ---------------- GPU memory ports: S_AXI_HP0/1/2 external, clocked by the GPU core clock ----------
# make_bd_intf_pins_external clones the PS7 pin (AXI3, 64-bit, 6-bit IDs); it returns nothing in
# 2026.1, so the new port is found by difference and renamed
proc make_ext {pin_path name} {
    set pin [get_bd_intf_pins $pin_path]
    set before [get_bd_intf_ports -quiet]
    make_bd_intf_pins_external $pin
    set new {}
    foreach q [get_bd_intf_ports -quiet] { if {[lsearch -exact $before $q] < 0} { lappend new $q } }
    if {[llength $new] != 1} { die 1 "make_bd_intf_pins_external $pin_path created '$new' (expected one port)" }
    set_property NAME $name [lindex $new 0]
    return [get_bd_intf_ports $name]
}
foreach n {0 1 2} {
    make_ext ps7/S_AXI_HP$n S_AXI_HP$n
    connect_bd_net $gclk [get_bd_pins ps7/S_AXI_HP${n}_ACLK]
}

# ---------------- GP0 interconnect: pl_regs, DMA registers, GPU registers ----------------
set ic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect gp0_ic]
set_property CONFIG.NUM_MI {3} $ic
connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0] [get_bd_intf_pins gp0_ic/S00_AXI]
foreach p {ps7/M_AXI_GP0_ACLK gp0_ic/ACLK gp0_ic/S00_ACLK gp0_ic/M00_ACLK gp0_ic/M01_ACLK} {
    connect_bd_net $fclk0 [get_bd_pins $p]
}
connect_bd_net $ia [get_bd_pins gp0_ic/ARESETN]
foreach p {S00_ARESETN M00_ARESETN M01_ARESETN} { connect_bd_net $pa [get_bd_pins gp0_ic/$p] }
connect_bd_net $gclk [get_bd_pins gp0_ic/M02_ACLK]
connect_bd_net [get_bd_pins rst_gpu/peripheral_aresetn] [get_bd_pins gp0_ic/M02_ARESETN]

# ---------------- pl_regs on GP0 (M00) ----------------
set regs [create_bd_cell -type module -reference pl_regs regs]
connect_bd_intf_net [get_bd_intf_pins gp0_ic/M00_AXI] [get_bd_intf_pins regs/s_axi]
connect_bd_net $fclk0 [get_bd_pins regs/aclk]
connect_bd_net $pa [get_bd_pins regs/aresetn]
# LED1/LED2 belong to the GPU in this design (its heartbeat is its bring-up signal): pl_regs'
# led1/led2 outputs stay unconnected
foreach p {fan_pwm} {
    create_bd_port -dir O $p; connect_bd_net [get_bd_pins regs/$p] [get_bd_ports $p]
}
foreach p {key1_n key2_n fan_tach} {
    create_bd_port -dir I $p; connect_bd_net [get_bd_ports $p] [get_bd_pins regs/$p]
}

# ---------------- AXI DMA (GP0 M01) + matrix engine + HP3 ----------------
set dma [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma dma]
set dma_cfg {c_include_sg 0 c_sg_length_width 26 c_addr_width 32 c_include_mm2s 1 c_include_s2mm 1
             c_m_axi_mm2s_data_width 64 c_m_axis_mm2s_tdata_width 64 c_mm2s_burst_size 16
             c_m_axi_s2mm_data_width 64 c_s_axis_s2mm_tdata_width 64 c_s2mm_burst_size 16}
set l {}
foreach {k v} $dma_cfg { lappend l CONFIG.$k $v }
must "configure axi_dma" { set_property -dict $l $dma }
foreach {k v} $dma_cfg {
    set got [get_property CONFIG.$k $dma]
    if {$got ne $v} { die 4 "DMA CHECK: $k = '$got', want '$v'" }
}
say "dma: [get_property VLNV $dma], simple mode, 64-bit streams, 26-bit length; intf [get_bd_intf_pins -of $dma]"
connect_bd_intf_net [get_bd_intf_pins gp0_ic/M01_AXI] [get_bd_intf_pins dma/S_AXI_LITE]
foreach p {s_axi_lite_aclk m_axi_mm2s_aclk m_axi_s2mm_aclk} { connect_bd_net $fclk0 [get_bd_pins dma/$p] }
connect_bd_net $pa [get_bd_pins dma/axi_resetn]

set mic [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect mem_ic]
set_property -dict [list CONFIG.NUM_SI {2} CONFIG.NUM_MI {1}] $mic
connect_bd_intf_net [get_bd_intf_pins dma/M_AXI_MM2S] [get_bd_intf_pins mem_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins dma/M_AXI_S2MM] [get_bd_intf_pins mem_ic/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins mem_ic/M00_AXI] [get_bd_intf_pins ps7/S_AXI_HP3]
foreach p {ps7/S_AXI_HP3_ACLK mem_ic/ACLK mem_ic/S00_ACLK mem_ic/S01_ACLK mem_ic/M00_ACLK} {
    connect_bd_net $fclk0 [get_bd_pins $p]
}
connect_bd_net $ia [get_bd_pins mem_ic/ARESETN]
foreach p {S00_ARESETN S01_ARESETN M00_ARESETN} { connect_bd_net $pa [get_bd_pins mem_ic/$p] }

set gemv [create_bd_cell -type module -reference zaccel_gemv gemv]
say "gemv intf: [get_bd_intf_pins -of $gemv]  pins: [get_bd_pins -of $gemv]"
connect_bd_intf_net [get_bd_intf_pins dma/M_AXIS_MM2S] [get_bd_intf_pins gemv/s_axis]
connect_bd_intf_net [get_bd_intf_pins gemv/m_axis] [get_bd_intf_pins dma/S_AXIS_S2MM]
connect_bd_net $fclk0 [get_bd_pins gemv/aclk]
# the engine is reset by the system reset AND by either DMA channel's reset (a DMACR soft reset
# before every job clears an aborted job): gemv_reset.v, registered AND in the FCLK0 domain
foreach p {mm2s_prmry_reset_out_n s2mm_prmry_reset_out_n} {
    if {[llength [get_bd_pins -quiet dma/$p]] != 1} { die 4 "DMA CHECK: axi_dma has no pin $p ([get_bd_pins -of $dma])" }
}
set grst [create_bd_cell -type module -reference gemv_reset gemv_rst]
connect_bd_net $fclk0 [get_bd_pins gemv_rst/aclk]
connect_bd_net $pa [get_bd_pins gemv_rst/peripheral_aresetn]
connect_bd_net [get_bd_pins dma/mm2s_prmry_reset_out_n] [get_bd_pins gemv_rst/mm2s_prmry_reset_out_n]
connect_bd_net [get_bd_pins dma/s2mm_prmry_reset_out_n] [get_bd_pins gemv_rst/s2mm_prmry_reset_out_n]
connect_bd_net [get_bd_pins gemv_rst/aresetn] [get_bd_pins gemv/aresetn]
set drv {}
foreach p [get_bd_pins -of [get_bd_nets -of [get_bd_pins gemv/aresetn]]] { if {$p ne "/gemv/aresetn"} { lappend drv $p } }
if {$drv ne "/gemv_rst/aresetn"} { die 4 "RESET CHECK: gemv/aresetn is driven by '$drv', want /gemv_rst/aresetn" }
say "gemv reset: gemv/aresetn <- gemv_rst (peripheral_aresetn & dma/mm2s_prmry_reset_out_n & dma/s2mm_prmry_reset_out_n)"

# ---------------- GPU register window: GP0 M02 -> port M_AXI_GPU (AXI3, the GPU is the slave) ------
set gport [create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_GPU]
set_property -dict [list CONFIG.PROTOCOL {AXI3} CONFIG.ADDR_WIDTH {32} CONFIG.DATA_WIDTH {32}] $gport
connect_bd_intf_net [get_bd_intf_pins gp0_ic/M02_AXI] $gport
set_property CONFIG.ASSOCIATED_BUSIF {M_AXI_GPU:S_AXI_HP0:S_AXI_HP1:S_AXI_HP2} $gclk

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
foreach p {tx_reset rx_reset} {
    if {[llength [get_bd_pins -quiet g2r/$p]]} { connect_bd_net [get_bd_pins rst_ps7_100M/peripheral_reset] [get_bd_pins g2r/$p] }
}

# ---------------- address map ----------------
set ds [get_bd_addr_spaces ps7/Data]
must "assign pl_regs 0x40000000/4K" {
    assign_bd_address -offset 0x40000000 -range 4K -target_address_space $ds [get_bd_addr_segs regs/s_axi/reg0] -force }
must "assign DMA 0x40400000/64K" {
    assign_bd_address -offset 0x40400000 -range 64K -target_address_space $ds [get_bd_addr_segs dma/S_AXI_LITE/Reg] -force }
must "assign GPU 0x43C00000/4K" {
    assign_bd_address -offset 0x43C00000 -range 4K -target_address_space $ds [get_bd_addr_segs M_AXI_GPU/Reg] -force }
assign_bd_address                                  ;# DMA and GPU master spaces -> DDR via their HP port

must "validate_bd_design" { validate_bd_design }
save_bd_design

# hard check: every address space, what it maps, and the three register windows + DDR reach
proc space_map {sp} {
    set m {}
    foreach s [get_bd_addr_segs -quiet -of_objects $sp] {
        set slave [get_bd_addr_segs -quiet -of_objects $s]
        lappend m [list $slave [get_property OFFSET $s] [get_property RANGE $s]]
    }
    return $m
}
set spaces [list [get_bd_addr_spaces ps7/Data] [get_bd_addr_spaces dma/Data_MM2S] [get_bd_addr_spaces dma/Data_S2MM]]
foreach n {0 1 2} { lappend spaces [get_bd_addr_spaces -of_objects [get_bd_intf_ports S_AXI_HP$n]] }
foreach sp $spaces { foreach e [space_map $sp] { say "address: $sp -> [lindex $e 0] offset [lindex $e 1] range [lindex $e 2]" } }
proc want_seg {sp slave off rng} {
    foreach e [space_map $sp] {
        if {[lindex $e 0] eq $slave} {
            if {[lindex $e 1] eq "" || [lindex $e 2] eq "" || [lindex $e 1] != $off || [lindex $e 2] != $rng} {
                die 4 "ADDRESS CHECK: $sp -> $slave at [lindex $e 1] range [lindex $e 2], want $off range $rng"
            }
            return
        }
    }
    die 4 "ADDRESS CHECK: $sp does not map $slave"
}
set psmap [space_map [get_bd_addr_spaces ps7/Data]]
if {[llength $psmap] != 3} { die 4 "ADDRESS CHECK: ps7/Data has [llength $psmap] segments, want 3 (pl_regs, DMA, GPU)" }
want_seg [get_bd_addr_spaces ps7/Data] [get_bd_addr_segs regs/s_axi/reg0]     0x40000000 0x1000
want_seg [get_bd_addr_spaces ps7/Data] [get_bd_addr_segs dma/S_AXI_LITE/Reg]   0x40400000 0x10000
want_seg [get_bd_addr_spaces ps7/Data] [get_bd_addr_segs M_AXI_GPU/Reg]        0x43C00000 0x1000
foreach sp {dma/Data_MM2S dma/Data_S2MM} {
    want_seg [get_bd_addr_spaces $sp] [get_bd_addr_segs ps7/S_AXI_HP3/HP3_DDR_LOWOCM] 0x00000000 0x40000000
}
foreach n {0 1 2} {
    want_seg [get_bd_addr_spaces -of_objects [get_bd_intf_ports S_AXI_HP$n]] \
             [get_bd_addr_segs ps7/S_AXI_HP$n/HP${n}_DDR_LOWOCM] 0x00000000 0x40000000
}
foreach n {0 1 2 3} {
    set p [get_bd_intf_pins ps7/S_AXI_HP$n]
    set clkp [get_bd_pins ps7/S_AXI_HP${n}_ACLK]
    say "HP$n: [get_property CONFIG.PROTOCOL $p] [get_property CONFIG.DATA_WIDTH $p]-bit ID [get_property CONFIG.ID_WIDTH $p] ACLK [get_property CONFIG.FREQ_HZ $clkp] Hz"
    if {[get_property CONFIG.DATA_WIDTH $p] != 64} { die 4 "HP CHECK: S_AXI_HP$n is [get_property CONFIG.DATA_WIDTH $p]-bit" }
}
foreach {port hz} {M_AXI_GPU 148750000 S_AXI_HP0 148750000 S_AXI_HP1 148750000 S_AXI_HP2 148750000} {
    set f [get_property CONFIG.FREQ_HZ [get_bd_intf_ports $port]]
    say "port $port: [get_property CONFIG.PROTOCOL [get_bd_intf_ports $port]] ID [get_property CONFIG.ID_WIDTH [get_bd_intf_ports $port]] FREQ_HZ $f"
    if {$f != $hz} { die 4 "CLOCK CHECK: $port FREQ_HZ $f, want $hz" }
}
say "address map check: pl_regs 0x40000000/4K, DMA 0x40400000/64K, GPU 0x43C00000/4K; DMA and GPU reach DDR 0-1G via HP3 / HP0-2"

set bdf [get_files system.bd]
generate_target all $bdf
set wrapper [make_wrapper -files $bdf -top]
add_files -norecurse $wrapper
set_property top system_top [current_fileset]
update_compile_order -fileset sources_1

# hard check: the generated wrapper's ports are exactly the ones system_top.v connects on u_sys
proc read_text {f} { set fh [open $f r]; set t [read $fh]; close $fh; regsub -all {/\*.*?\*/} $t " " t; regsub -all {//[^\n]*} $t " " t; return $t }
set wt [read_text $wrapper]
set wports {}
foreach {all dir name} [regexp -all -inline {\m(input|output|inout)\M\s*(?:\[[^]]*\])?\s*([A-Za-z_][A-Za-z0-9_]*)\s*;} $wt] { lappend wports $name }
set tt [read_text $here/system_top.v]
set s [string first "system_wrapper u_sys" $tt]
set e [string first ");" $tt $s]
set tports {}
foreach {all name} [regexp -all -inline {\.([A-Za-z_][A-Za-z0-9_]*)\s*\(} [string range $tt $s $e]] { lappend tports $name }
set diffs {}
foreach p $wports { if {[lsearch -exact $tports $p] < 0} { lappend diffs "only in wrapper: $p" } }
foreach p $tports { if {[lsearch -exact $wports $p] < 0} { lappend diffs "only in system_top.v: $p" } }
say "wrapper ports: [llength $wports], system_top.v u_sys connections: [llength $tports]"
if {[llength $diffs]} { foreach d $diffs { say "  $d" }; die 5 "system_wrapper ($wrapper) and system_top.v u_sys differ" }

# ---------------- implement ----------------
set impl [get_runs impl_1]
# the GPU core closes 148.75 MHz with ~0.15 ns to spare on its own (accel/gpu/fpga/build.tcl uses the
# same Explore directives); keep them here
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE Explore $impl
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Explore $impl
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true $impl
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE Explore $impl
set_property STEPS.ROUTE_DESIGN.ARGS.DIRECTIVE Explore $impl
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true $impl
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE Explore $impl

launch_runs synth_1 -jobs 8
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] ne "100%"} { die 1 "SYNTH FAILED ([get_property STATUS [get_runs synth_1]])" }
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
if {[get_property PROGRESS $impl] ne "100%"} { die 1 "IMPL FAILED ([get_property STATUS $impl])" }
open_run impl_1

# hard check: no package ball carries two ports
set balls [dict create]
foreach p [get_ports] {
    set b [get_property PACKAGE_PIN $p]
    if {$b eq ""} { continue }
    if {[dict exists $balls $b]} { die 6 "ball $b used by [dict get $balls $b] and $p" }
    dict set balls $b $p
}
say "package balls: [dict size $balls] ports placed, no ball used twice"

report_utilization    -file $out/system_util.rpt
report_utilization    -hierarchical -hierarchical_depth 3 -file $out/system_util_hier.rpt
report_timing_summary -delay_type min_max -max_paths 20 -report_unconstrained -check_timing_verbose \
                      -file $out/system_timing.rpt
report_clocks         -file $out/system_clocks.rpt
report_clock_interaction -delay_type min_max -file $out/system_clock_interaction.rpt
report_drc            -file $out/system_drc.rpt
report_methodology    -file $out/system_methodology.rpt
report_bus_skew       -max_paths 4 -file $out/system_bus_skew.rpt
catch {report_cdc -details -file $out/system_cdc.rpt}

set sum {}
proc note {m} { upvar #0 sum sum; lappend sum $m; say $m }
set ts [report_timing_summary -no_detailed_paths -return_string]
set met [string match "*All user specified timing constraints are met.*" $ts]
set wns [get_property SLACK [get_timing_paths -delay_type max -max_paths 1 -nworst 1]]
set whs [get_property SLACK [get_timing_paths -delay_type min -max_paths 1 -nworst 1]]
note "timing: WNS=$wns ns WHS=$whs ns TNS=[get_property STATS.TNS $impl] THS=[get_property STATS.THS $impl] ([expr {$met ? {all constraints met} : {CONSTRAINTS NOT MET}}])"
set failed [expr {!$met}]
if {![string is double -strict $wns] || $wns < 0} { set failed 1 }
if {![string is double -strict $whs] || $whs < 0} { set failed 1 }
# worst setup / hold slack of the paths ending at each clock (intra- and inter-clock)
foreach c [lsort [get_clocks]] {
    set s "-"; set h "-"
    catch {set s [get_property SLACK [get_timing_paths -to $c -delay_type max -max_paths 1 -nworst 1]]}
    catch {set h [get_property SLACK [get_timing_paths -to $c -delay_type min -max_paths 1 -nworst 1]]}
    if {$s eq ""} { set s "-" }
    if {$h eq ""} { set h "-" }
    note [format "clock %-28s period %7.3f ns  WNS %7s ns  WHS %7s ns" $c [get_property PERIOD $c] $s $h]
    if {([string is double -strict $s] && $s < 0) || ([string is double -strict $h] && $h < 0)} { set failed 1 }
}
# bus skew: the GPU scanout FIFO's gray pointers and the IP clock converters
set bs [report_bus_skew -max_paths 4 -return_string]
set in_sum 0; set nbs 0
foreach line [split $bs "\n"] {
    if {[string match "1. Bus Skew Report Summary*" $line]} { set in_sum 1; continue }
    if {[string match "2. Bus Skew Report Per Constraint*" $line]} { set in_sum 0; continue }
    if {$in_sum && [regexp {^\s*(Slow|Fast)\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+|inf)\s*$} $line -> corner req act slk]} {
        incr nbs
        if {![string is double -strict $slk] || $slk < 0} { set failed 1; note "BUS SKEW FAIL: $line" }
    }
}
note "bus skew: $nbs constraint rows checked"
set u [report_utilization -return_string]
foreach c {"Slice LUTs" "Slice Registers" "Block RAM Tile" "DSPs" "Bonded IOB" "BUFGCTRL" "MMCME2_ADV" "IDELAYE2"} {
    if {[regexp "\\|\\s*${c}\\*?\\s*\\|\\s*(\[0-9.\]+)\\s*\\|\[^|\]*\\|\[^|\]*\\|\\s*(\[0-9.\]+)" $u -> used avail]} { note "util $c: $used / $avail" }
}
# critical warnings of the synth/impl runs
set cw {}
foreach run {synth_1 impl_1} {
    set lf [file join [get_property DIRECTORY [get_runs $run]] runme.log]
    if {![file exists $lf]} { continue }
    set fh [open $lf r]
    foreach l [split [read $fh] "\n"] { if {[string match "CRITICAL WARNING*" $l]} { lappend cw "$run: $l" } }
    close $fh
}
set cw [lsort -unique $cw]
note "critical warnings in synth/impl runs: [llength $cw]"
foreach l $cw { note "  $l" }

# ps7_init: the SPL's copy lives in ../ps7-vivado (unzipped from the XSA, VIVADO.md section 4);
# say whether this design changed it
file mkdir $out/ps7_init
foreach f [glob -nocomplain $here/prj/system.gen/sources_1/bd/system/ip/*ps7*/ps7_init*] { file copy -force $f $out/ps7_init/ }
foreach f {ps7_init_gpl.c ps7_init_gpl.h ps7_init.tcl} {
    set a $out/ps7_init/$f; set b $root/ps7-vivado/$f
    if {![file exists $a]} { note "ps7_init: $f not generated"; continue }
    if {![file exists $b]} { note "ps7_init: $f: no copy in ps7-vivado/"; continue }
    set same [expr {[read_text $a] eq [read_text $b]}]
    note "ps7_init: $f [expr {$same ? {identical to} : {DIFFERS from}}] ps7-vivado/$f"
}

set fh [open $out/system_summary.txt w]
puts $fh "PZ7020-StarLite system build [clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S}], Vivado [version -short]"
puts $fh "zaccel_gemv.v: $gemv_stamp"
foreach l $sum { puts $fh $l }
close $fh

if {$failed} {
    set fh [open $out/TIMING_FAILED.txt w]; foreach l $sum { puts $fh $l }; close $fh
    die 2 "TIMING NOT MET -- no system.bit / system.xsa written; see build/system_timing.rpt"
}
set bit [glob $here/prj/system.runs/impl_1/*.bit]
file copy -force $bit $out/system.bit
write_hw_platform -fixed -include_bit -force -file $out/system.xsa
say "bitstream: $out/system.bit [file size $out/system.bit] B"
say "xsa: $out/system.xsa [file size $out/system.xsa] B"
say "DONE"
