# check_routed.tcl -- the ROUTED design the card's bitstream was written from (post-route phys_opt, the
# last checkpoint before write_bitstream), queried for what the FCLK0 experiment relies on:
#   which clock actually reaches pl_regs' counter flip-flops, the GP0 interconnect, and the PS7's GP0 port;
#   whether anything in the fabric gates FCLK0 (a BUFGCE or similar) between the PS7 and those loads.
#     D:\2026.1\Vivado\bin\vivado.bat -mode batch -source hardware\pz7020-starlite\vivado\check_routed.tcl
set here [file dirname [file normalize [info script]]]
open_checkpoint $here/prj/system.runs/impl_1/system_top_postroute_physopt.dcp
set fails 0
proc check {what ok got} {
    global fails
    if {$ok} { puts "  PASS  $what  ($got)" } else { puts "  FAIL  $what  ($got)"; incr fails }
}
# pl_regs' 64-bit counter: its flip-flops and the clock that reaches them
set tick [get_cells -hierarchical -filter {NAME =~ *regs*/tick_reg[0] || NAME =~ *regs*tick_reg[0]}]
check "pl_regs counter flip-flop found" [expr {[llength $tick] == 1}] $tick
set tclk [get_clocks -of_objects [get_pins $tick/C]]
check "the counter is clocked by clk_fpga_0 (FCLK0)" [expr {$tclk eq "clk_fpga_0"}] $tclk
set ntick [llength [get_cells -hierarchical -filter {NAME =~ *regs*/tick_reg[*]}]]
check "the counter is 64 flip-flops wide" [expr {$ntick == 64}] $ntick
# the PS7's GP0 port clock pin
set ps [get_cells -hierarchical -filter {REF_NAME == PS7}]
check "one PS7 cell" [expr {[llength $ps] == 1}] $ps
set gpclk [get_clocks -of_objects [get_pins $ps/MAXIGP0ACLK]]
check "the PS7's M_AXI_GP0_ACLK pin is clk_fpga_0" [expr {$gpclk eq "clk_fpga_0"}] $gpclk
# the path from the PS7's FCLKCLK[0] output to the counter: only a global buffer, no gate
set fnet [get_nets -of_objects [get_pins $ps/FCLKCLK[0]]]
set drv [get_cells -of_objects [get_pins -leaf -of_objects [get_nets -segments [get_nets -of_objects [get_pins $tick/C]]] -filter {DIRECTION == OUT}]]
set dref [get_property REF_NAME $drv]
check "the counter's clock is driven by a plain BUFG (no clock enable in the fabric)" [expr {$dref eq "BUFG"}] "$drv $dref"
set bin [get_nets -of_objects [get_pins $drv/I]]
set bsrc [get_cells -of_objects [get_pins -leaf -of_objects [get_nets -segments $bin] -filter {DIRECTION == OUT}]]
check "that BUFG's input comes straight from the PS7's FCLKCLK" [expr {$bsrc eq $ps}] "$bin <- $bsrc"
# every clock-buffer type in the design, to see whether any could gate FCLK0
foreach t {BUFGCE BUFGMUX BUFGCTRL BUFHCE BUFR} {
    set c [get_cells -hierarchical -quiet -filter "REF_NAME == $t"]
    puts "  info  $t cells: [llength $c]"
}
# loads of clk_fpga_0: how many flip-flops stop when FCLK0 stops
set loads [llength [get_cells -hierarchical -filter {IS_SEQUENTIAL && PRIMITIVE_LEVEL == LEAF} -quiet]]
puts "  info  sequential cells in the design: $loads"
puts [expr {$fails ? "ROUTED CHECK: FAIL ($fails)" : "ROUTED CHECK: PASS"}]
