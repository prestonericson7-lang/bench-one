# ooc_core.tcl -- Vivado out-of-context synthesis (+ optional place & route) of the render core.
#
#   cd sim/core_ooc
#   vivado -mode batch -nojournal -log ooc.log -source ooc_core.tcl [-tclargs synth|impl [rtl_dir [out_dir]]]
#
# Reports (sim/core_ooc/out/): util_synth.rpt, util_hier_synth.rpt, ram_synth.rpt,
# timing_synth.rpt and, for 'impl', util_routed.rpt, timing_routed.rpt, ram_routed.rpt.
# Clock: core 148.75 MHz (create_clock -period 6.723 on clk, core_ooc.xdc). Ports are left
# without I/O delays (reg-to-reg paths are what the core owns; the ports connect to registers
# in gpu_top's other modules).
set mode impl
if {[llength $argv] > 0} { set mode [lindex $argv 0] }
set here [file dirname [file normalize [info script]]]
set rtl  [file normalize [file join $here .. .. rtl]]
set out  [file join $here out]
# optional: -tclargs <mode> <rtl dir> <out dir>  (e.g. a candidate variant of the core)
if {[llength $argv] > 1} { set rtl [file normalize [lindex $argv 1]] }
if {[llength $argv] > 2} { set out [file normalize [lindex $argv 2]] }
puts "OOC rtl=$rtl out=$out mode=$mode"
file mkdir $out
set part xc7z020clg400-2
set files {core_top.v core_collector.v core_listram.v core_fetch.v core_frame.v core_tri.v
           core_sprite.v core_writer.v core_ram.v core_ram2.v core_fifo.v}
foreach f $files { read_verilog [file join $rtl $f] }
read_xdc -mode out_of_context [file join $here core_ooc.xdc]
set_param general.maxThreads 4
synth_design -top core_top -part $part -mode out_of_context -flatten_hierarchy rebuilt
report_utilization -file [file join $out util_synth.rpt]
report_utilization -hierarchical -file [file join $out util_hier_synth.rpt]
report_ram_utilization -file [file join $out ram_synth.rpt]
report_timing_summary -max_paths 20 -file [file join $out timing_synth.rpt]
puts "OOC_SYNTH_DONE"
if {$mode eq "impl"} {
    opt_design
    place_design
    phys_opt_design
    route_design
    phys_opt_design
    report_utilization -file [file join $out util_routed.rpt]
    report_utilization -hierarchical -file [file join $out util_hier_routed.rpt]
    report_ram_utilization -file [file join $out ram_routed.rpt]
    report_timing_summary -max_paths 30 -file [file join $out timing_routed.rpt]
    report_route_status -file [file join $out route_status.rpt]
    set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
    set whs [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -hold]]
    puts "OOC_IMPL_DONE WNS=$wns WHS=$whs"
    # I/O budget check (information only, after routing): ports driven / sampled by registers in
    # gpu_top's other modules, 1.5 ns of the 6.723 ns period each side
    set ins  [lsearch -all -inline -not -exact [get_ports -filter {DIRECTION == IN}] clk]
    set_input_delay  -clock clk 1.5 $ins
    set_output_delay -clock clk 1.5 [get_ports -filter {DIRECTION == OUT}]
    report_timing_summary -max_paths 10 -file [file join $out timing_routed_io.rpt]
    set wio [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
    puts "OOC_IO_CHECK WNS_with_io_delays=$wio"
}
