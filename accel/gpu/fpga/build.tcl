# =============================================================================================
# fpga/build.tcl -- Vivado batch build of the FPGA-GPU bitstream (Puzhi PZ7020, xc7z020clg400-2)
#
#   cd fpga && vivado -mode batch -source build.tcl [-tclargs jobs=N]
#   (fpga/build_fpga.cmd runs exactly this with D:\2026.1\Vivado\bin\vivado.bat)
#
# Outputs (project root out/):  pl.bit, pl.bin, utilization.rpt, timing_summary.rpt,
#   utilization_hier.rpt, clocks.rpt, clock_interaction.rpt, bus_skew.rpt, cdc.rpt, drc.rpt,
#   methodology.rpt, build_summary.txt (incl. WNS/WHS per clock), and TIMING_FAILED.txt when
#   WNS, WHS or a bus-skew slack is < 0 or a bus-skew constraint misses a pointer bit.
# Exit code: 0 = bitstream written, timing met, no critical warnings,
#            2 = bitstream written but timing NOT met,
#            3 = bitstream written, timing met, but CRITICAL WARNINGs were issued (see summary),
#            1 = build failed (message "*** [build] FAILED at step ...").
#
# Every step prints "=== [build] step k/9: ... ===" so a failure is easy to localise.
#   1 project          fpga/build/pz7020_gpu.xpr (deleted and recreated each run)
#   2 block design     ps7_bd = processing_system7 with M_AXI_GP0 + S_AXI_HP0/1/2 (64-bit),
#                      external interface ports M_AXI_GP0 / S_AXI_HP0/1/2, external clock ports
#                      M_AXI_GP0_ACLK / S_AXI_HP0_ACLK / S_AXI_HP1_ACLK / S_AXI_HP2_ACLK
#                      (148.75 MHz = PL core clock), DDR + FIXED_IO; GP0 window 0x43C00000 / 4 KB;
#                      validate. This is the construction the real Vivado 2026.1 run in
#                      fpga/probe/ps7_bd_create.tcl proved (no critical warnings; its wrapper
#                      port list is fpga/probe/ps7_bd_wrapper_ports.txt).
#   3 wrapper          generate targets, make_wrapper, compare its port list with
#                      rtl/sim_stubs/ps7_bd_wrapper.v (the stand-in used for lint/simulation)
#   4 sources          rtl/*.v (NOT rtl/sim_stubs), rtl/gpu_defs.vh, fpga/pz7020_gpu.xdc
#   5 synthesis        6 implementation       7 reports       8 bitstream     9 timing verdict
#
# The PS itself is initialised by U-Boot SPL (its own ps7_init), so the DDR / MIO settings made
# here only have to be valid; they do not end up in the bitstream. They follow the Puzhi PZ7020
# manual anyway (QSPI MIO 1-6, UART0 MIO 10/11, ENET0 RGMII MIO 16-27 + MDIO 52/53, USB0 MIO
# 28-39, SD0 MIO 40-45, bank 0 3.3 V, bank 1 1.8 V, DDR3L 16-bit MT41K256M16). The design uses no
# FCLK and no PS reset: all AXI clocks come from the PL MMCM (see rtl/clkgen.v).
#
# Any CRITICAL WARNING issued in this Vivado session (block design, synthesis, implementation,
# constraints) is counted and listed in build_summary.txt: a critical warning usually means a
# constraint or a BD setting was silently dropped.
# =============================================================================================

set script_dir [file normalize [file dirname [info script]]]
set root_dir   [file normalize [file join $script_dir ..]]
set rtl_dir    [file join $root_dir rtl]
set stub_file  [file join $rtl_dir sim_stubs ps7_bd_wrapper.v]
set out_dir    [file join $root_dir out]
set build_dir  [file join $script_dir build]
set xdc_file   [file join $script_dir pz7020_gpu.xdc]
set part       xc7z020clg400-2
set proj_name  pz7020_gpu
set bd_name    ps7_bd
set top_name   gpu_top
set core_hz    148750000   ;# PL core clock = all four AXI ACLKs (SPEC 2)
set nsteps     9

set jobs 4
if {[info exists ::env(NUMBER_OF_PROCESSORS)] && [string is integer -strict $::env(NUMBER_OF_PROCESSORS)]} {
    set jobs [expr {min(8, max(1, $::env(NUMBER_OF_PROCESSORS)))}]
}
foreach a $argv {
    if {[regexp {^jobs=(\d+)$} $a -> n]} { set jobs $n }
}

set cur_step "init"

proc banner {k msg} {
    global nsteps cur_step
    set cur_step "$k/$nsteps $msg"
    puts ""
    puts "=== \[build\] step $k/$nsteps: $msg ==="
    flush stdout
}
proc info_msg {msg} { puts "\[build\] $msg"; flush stdout }
proc warn {msg} {
    puts "\[build\] WARNING: $msg"
    flush stdout
}
proc die {msg} {
    global cur_step
    puts ""
    puts "*** \[build\] FAILED at step $cur_step ***"
    puts "*** $msg"
    puts ""
    flush stdout
    exit 1
}
# run a script in the caller's scope; any Tcl error aborts the build with a clear message
proc must {what script} {
    if {[catch {uplevel 1 $script} err]} {
        die "$what: $err"
    }
}
# run a script in the caller's scope; errors only warn (for settings that cannot matter)
proc may {what script} {
    if {[catch {uplevel 1 $script} err]} {
        warn "$what failed (continuing): $err"
        return 0
    }
    return 1
}

# print the tail of a run's log and every ERROR line in it
proc dump_run_log {run} {
    set dir [get_property DIRECTORY [get_runs $run]]
    foreach lf [list [file join $dir runme.log]] {
        if {![file exists $lf]} { continue }
        set fh [open $lf r]; set lines [split [read $fh] "\n"]; close $fh
        puts "----- ERROR/CRITICAL lines of $lf -----"
        foreach l $lines {
            if {[regexp {^(ERROR|CRITICAL WARNING)} $l]} { puts $l }
        }
        puts "----- last 60 lines of $lf -----"
        foreach l [lrange $lines end-60 end] { puts $l }
        puts "-----"
    }
}

proc check_run {run} {
    set r [get_runs $run]
    set st [get_property STATUS $r]
    set pr [get_property PROGRESS $r]
    info_msg "$run: STATUS='$st' PROGRESS=$pr"
    if {$pr ne "100%" || [string match -nocase "*error*" $st] || [string match -nocase "*fail*" $st]} {
        dump_run_log $run
        die "$run did not complete (STATUS '$st', PROGRESS $pr)"
    }
}

# --------------------------------------------------------------------------------------------
# Verilog port list extraction (for the wrapper vs stub comparison)
#   returns a dict  name -> "direction width"   (width "" for 1-bit ports)
# --------------------------------------------------------------------------------------------
proc verilog_ports {file header_only} {
    set fh [open $file r]; set txt [read $fh]; close $fh
    regsub -all {/\*.*?\*/} $txt " " txt
    regsub -all {//[^\n]*} $txt " " txt
    set s [string first "module ps7_bd_wrapper" $txt]
    if {$s < 0} { return -code error "no 'module ps7_bd_wrapper' in $file" }
    set txt [string range $txt $s end]
    if {$header_only} {
        # ANSI stub: ports are declared in the header, up to the first ');'
        set e [string first ");" $txt]
        set txt [string range $txt 0 $e]
    }
    set ports [dict create]
    foreach {all dir width name} [regexp -all -inline \
            {\m(input|output|inout)\M\s+(?:wire\s+|reg\s+)?(\[[^]]*\])?\s*([A-Za-z_][A-Za-z0-9_]*)} $txt] {
        regsub -all {\s} $width "" width
        dict set ports $name [string trim "$dir $width"]
    }
    return $ports
}

proc find_file {dir name} {
    foreach f [glob -nocomplain -directory $dir -types f $name] { return $f }
    foreach d [glob -nocomplain -directory $dir -types d *] {
        set r [find_file $d $name]
        if {$r ne ""} { return $r }
    }
    return ""
}

# ============================================================================================
banner 1 "create project ($part) in $build_dir"
# ============================================================================================
info_msg "Vivado [version -short], jobs=$jobs, root=$root_dir"
catch {close_project}
if {[file exists $build_dir]} {
    if {[catch {file delete -force $build_dir} err]} {
        warn "could not delete $build_dir ($err); create_project -force will overwrite"
    }
}
file mkdir $build_dir
file mkdir $out_dir
foreach f {pl.bit pl.bin utilization.rpt utilization_hier.rpt timing_summary.rpt clocks.rpt
           clock_interaction.rpt bus_skew.rpt cdc.rpt drc.rpt methodology.rpt build_summary.txt
           TIMING_FAILED.txt} {
    catch {file delete -force [file join $out_dir $f]}
}
must "create_project" {
    create_project -force $proj_name $build_dir -part $part
    set_property target_language Verilog [current_project]
    set_property default_lib xil_defaultlib [current_project]
}

# ============================================================================================
banner 2 "block design $bd_name (processing_system7, GP0 + HP0/1/2)"
# ============================================================================================
must "create_bd_design" { create_bd_design $bd_name }

set ps7_vlnv "xilinx.com:ip:processing_system7:5.5"
set defs [lsort -dictionary [get_ipdefs -all -quiet xilinx.com:ip:processing_system7:*]]
if {[llength $defs] > 0} { set ps7_vlnv [lindex $defs end] }
info_msg "processing_system7 IP: $ps7_vlnv"
must "create_bd_cell processing_system7" {
    set ps [create_bd_cell -type ip -vlnv $ps7_vlnv processing_system7_0]
}

# Order and values as in fpga/probe/ps7_bd_create.tcl (proved with Vivado 2026.1).
# MIO bank voltages first (RGMII on bank 1 needs 1.8 V)
may "MIO bank voltages 0 = 3.3 V, 1 = 1.8 V" {
    set_property -dict [list \
        CONFIG.PCW_PRESET_BANK0_VOLTAGE {LVCMOS 3.3V} \
        CONFIG.PCW_PRESET_BANK1_VOLTAGE {LVCMOS 1.8V} \
    ] $ps
}
# essential configuration: the AXI ports this design uses
must "configure PS7 AXI ports" {
    set_property -dict [list \
        CONFIG.PCW_USE_M_AXI_GP0         {1} \
        CONFIG.PCW_USE_S_AXI_HP0         {1} \
        CONFIG.PCW_USE_S_AXI_HP1         {1} \
        CONFIG.PCW_USE_S_AXI_HP2         {1} \
        CONFIG.PCW_S_AXI_HP0_DATA_WIDTH  {64} \
        CONFIG.PCW_S_AXI_HP1_DATA_WIDTH  {64} \
        CONFIG.PCW_S_AXI_HP2_DATA_WIDTH  {64} \
    ] $ps
}
# no FCLK / FCLK_RESET pins: the PL has its own MMCM and resets
may "disable FCLK_CLK0 port"     { set_property CONFIG.PCW_EN_CLK0_PORT {0} $ps }
may "disable FCLK_RESET0_N port" { set_property CONFIG.PCW_EN_RST0_PORT {0} $ps }
# DDR3L (only has to validate: U-Boot SPL initialises the real controller)
may "DDR type DDR 3 (Low Voltage)" { set_property CONFIG.PCW_UIPARAM_DDR_MEMORY_TYPE {DDR 3 (Low Voltage)} $ps }
may "DDR part MT41K256M16 RE-125"  { set_property CONFIG.PCW_UIPARAM_DDR_PARTNO {MT41K256M16 RE-125} $ps }
may "DDR bus width 16 Bit"         { set_property CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH {16 Bit} $ps }
# MIO peripherals (Puzhi PZ7020 manual; validation/documentation only, the SPL owns the real setup)
may "QSPI single SS (MIO 1..6)" {
    set_property -dict [list \
        CONFIG.PCW_QSPI_PERIPHERAL_ENABLE    {1} \
        CONFIG.PCW_QSPI_GRP_SINGLE_SS_ENABLE {1} \
        CONFIG.PCW_QSPI_GRP_SINGLE_SS_IO     {MIO 1 .. 6} \
    ] $ps
}
may "UART0 (MIO 10..11)" {
    set_property -dict [list CONFIG.PCW_UART0_PERIPHERAL_ENABLE {1} CONFIG.PCW_UART0_UART0_IO {MIO 10 .. 11}] $ps
}
may "ENET0 RGMII (MIO 16..27)" {
    set_property -dict [list CONFIG.PCW_ENET0_PERIPHERAL_ENABLE {1} CONFIG.PCW_ENET0_ENET0_IO {MIO 16 .. 27}] $ps
}
may "ENET0 MDIO (MIO 52..53)" {
    set_property -dict [list CONFIG.PCW_ENET0_GRP_MDIO_ENABLE {1} CONFIG.PCW_ENET0_GRP_MDIO_IO {MIO 52 .. 53}] $ps
}
may "USB0 (MIO 28..39)" {
    set_property -dict [list CONFIG.PCW_USB0_PERIPHERAL_ENABLE {1} CONFIG.PCW_USB0_USB0_IO {MIO 28 .. 39}] $ps
}
may "SD0 (MIO 40..45)" {
    set_property -dict [list CONFIG.PCW_SD0_PERIPHERAL_ENABLE {1} CONFIG.PCW_SD0_SD0_IO {MIO 40 .. 45}] $ps
}
# read back what matters (a silently ignored value shows up here)
foreach k {PCW_UART0_UART0_IO PCW_QSPI_GRP_SINGLE_SS_IO PCW_ENET0_ENET0_IO PCW_ENET0_GRP_MDIO_IO
           PCW_USB0_USB0_IO PCW_SD0_SD0_IO PCW_PRESET_BANK0_VOLTAGE PCW_PRESET_BANK1_VOLTAGE
           PCW_UIPARAM_DDR_MEMORY_TYPE PCW_UIPARAM_DDR_PARTNO PCW_UIPARAM_DDR_BUS_WIDTH
           PCW_EN_CLK0_PORT PCW_EN_RST0_PORT} {
    set v "?"
    catch {set v [get_property CONFIG.$k $ps]}
    info_msg "PS7 $k = $v"
}

# DDR and FIXED_IO external (no board preset: there is no board file for the PZ7020)
must "apply_bd_automation (DDR, FIXED_IO)" {
    apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
        -config {make_external "FIXED_IO, DDR" apply_board_preset "0" Master "Disable" Slave "Disable"} $ps
}
foreach p {DDR FIXED_IO} {
    if {[get_bd_intf_ports -quiet $p] eq ""} {
        warn "automation did not create port $p; making it external by hand"
        must "make $p external" {
            make_bd_intf_pins_external [get_bd_intf_pins processing_system7_0/$p]
            set_property NAME $p [get_bd_intf_ports ${p}_0]
        }
    }
}

# External AXI interface ports, cloned from the PS7 pins (so the wrapper carries exactly the
# pins' signals) and named like the pins: M_AXI_GP0 (Master: the PL is its slave),
# S_AXI_HP0/1/2 (Slave: the PL is the master).
proc make_ext_intf {pin_name} {
    set pin [get_bd_intf_pins -quiet processing_system7_0/$pin_name]
    if {$pin eq ""} { die "PS7 interface pin $pin_name does not exist (PS7 port not enabled?)" }
    set before [get_bd_intf_ports -quiet]
    make_bd_intf_pins_external $pin
    set new {}
    foreach q [get_bd_intf_ports -quiet] { if {[lsearch -exact $before $q] < 0} { lappend new $q } }
    if {[llength $new] != 1} { die "make_bd_intf_pins_external $pin_name created '$new' (expected one port)" }
    set_property NAME $pin_name [lindex $new 0]
    set p [get_bd_intf_ports $pin_name]
    info_msg "interface port $pin_name <- processing_system7_0/$pin_name (mode [get_property MODE $p])"
    return $p
}
must "external interface ports" {
    foreach pin {M_AXI_GP0 S_AXI_HP0 S_AXI_HP1 S_AXI_HP2} { make_ext_intf $pin }
}
# All four AXI clocks are PL inputs (the core clock). FREQ_HZ goes on the clock port only
# (create_bd_port -freq_hz, else WARNING BD 5-670) together with ASSOCIATED_BUSIF; it propagates
# to the interface port during validation. Setting FREQ_HZ on the interface ports themselves is
# rejected by Vivado 2026.1 (CRITICAL WARNING BD 41-737 "read-only").
foreach pin {M_AXI_GP0 S_AXI_HP0 S_AXI_HP1 S_AXI_HP2} {
    must "clock port ${pin}_ACLK" {
        set cp [create_bd_port -dir I -type clk -freq_hz $core_hz ${pin}_ACLK]
        set_property CONFIG.ASSOCIATED_BUSIF $pin $cp
        connect_bd_net $cp [get_bd_pins processing_system7_0/${pin}_ACLK]
    }
}

# address map: GP0 register window 0x43C00000 / 4 KB; HP ports see DDR (auto)
must "assign M_AXI_GP0 0x43C00000/4K" {
    assign_bd_address -offset 0x43C00000 -range 4K \
        -target_address_space [get_bd_addr_spaces processing_system7_0/Data] \
        [get_bd_addr_segs M_AXI_GP0/Reg] -force
}
may "assign remaining addresses" { assign_bd_address }
foreach sp [get_bd_addr_spaces -quiet] {
    foreach sg [get_bd_addr_segs -quiet -of_objects $sp] {
        set off ""; set rng ""
        catch {set off [get_property OFFSET $sg]}
        catch {set rng [get_property RANGE $sg]}
        info_msg "address: $sg offset=$off range=$rng"
        if {[string match "*/Data/*GP0*" $sg]} {
            if {$off eq "" || [catch {expr {$off != 0x43C00000 || $rng != 0x1000}} bad] || $bad} {
                die "GP0 register window is at '$off' range '$rng', expected 0x43C00000 / 0x1000"
            }
        }
    }
}

must "validate_bd_design" { validate_bd_design }
must "save_bd_design"     { save_bd_design }
# the core clock frequency must have propagated to every AXI interface port
foreach pin {M_AXI_GP0 S_AXI_HP0 S_AXI_HP1 S_AXI_HP2} {
    set f ""
    catch {set f [get_property CONFIG.FREQ_HZ [get_bd_intf_ports $pin]]}
    info_msg "interface port $pin FREQ_HZ = $f"
    if {$f ne $core_hz} { die "FREQ_HZ of $pin is '$f', expected $core_hz" }
}

# ============================================================================================
banner 3 "generate block design + wrapper, compare with rtl/sim_stubs/ps7_bd_wrapper.v"
# ============================================================================================
set bd_file [get_files -quiet ${bd_name}.bd]
if {$bd_file eq ""} { die "block design file ${bd_name}.bd not found in the project" }
must "synth_checkpoint_mode None" { set_property synth_checkpoint_mode None $bd_file }
must "generate_target" { generate_target all $bd_file }
set wrapper ""
must "make_wrapper" { set wrapper [make_wrapper -files $bd_file -top] }
if {$wrapper eq "" || ![file exists $wrapper]} {
    set wrapper [find_file $build_dir ${bd_name}_wrapper.v]
}
if {$wrapper eq "" || ![file exists $wrapper]} { die "generated ${bd_name}_wrapper.v not found under $build_dir" }
info_msg "wrapper: $wrapper"
must "add wrapper" { add_files -norecurse $wrapper }

must "compare wrapper ports with the simulation stub" {
    set gen  [verilog_ports $wrapper 0]
    set stub [verilog_ports $stub_file 1]
    set diffs {}
    dict for {n v} $gen {
        if {![dict exists $stub $n]} {
            lappend diffs "only in generated wrapper: $v $n"
        } elseif {[dict get $stub $n] ne $v} {
            lappend diffs "differs: $n generated '$v' stub '[dict get $stub $n]'"
        }
    }
    dict for {n v} $stub {
        if {![dict exists $gen $n]} { lappend diffs "only in stub: $v $n" }
    }
    info_msg "wrapper ports: generated [dict size $gen], stub [dict size $stub]"
    if {[llength $diffs] > 0} {
        foreach d $diffs { puts "   $d" }
        error "[llength $diffs] port difference(s): update rtl/sim_stubs/ps7_bd_wrapper.v and the u_ps7 instance in rtl/gpu_top.v to the generated names ($wrapper)"
    }
}

# ============================================================================================
banner 4 "add RTL sources, header and constraints"
# ============================================================================================
set rtl_files [lsort [glob -nocomplain -directory $rtl_dir -types f *.v]]
set tails {}
foreach f $rtl_files { lappend tails [file tail $f] }
foreach req {gpu_top.v gpu_pl.v clkgen.v par_rx.v sync_fifo.v axi_gp_regs.v core_top.v scanout.v} {
    if {[lsearch -exact $tails $req] < 0} { die "missing rtl/$req" }
}
info_msg "RTL: $tails"
must "add RTL" {
    add_files -norecurse $rtl_files
    add_files -norecurse [file join $rtl_dir gpu_defs.vh]
    set_property file_type {Verilog Header} [get_files gpu_defs.vh]
    set_property include_dirs [list $rtl_dir] [get_filesets sources_1]
}
if {![file exists $xdc_file]} { die "constraints file $xdc_file missing" }
must "add XDC" { add_files -fileset constrs_1 -norecurse $xdc_file }
must "set top" {
    set_property top $top_name [get_filesets sources_1]
    update_compile_order -fileset sources_1
}

# ============================================================================================
banner 5 "synthesis"
# ============================================================================================
may "synthesis directive" {
    set_property STEPS.SYNTH_DESIGN.ARGS.DIRECTIVE Default [get_runs synth_1]
    set_property STEPS.SYNTH_DESIGN.ARGS.FLATTEN_HIERARCHY rebuilt [get_runs synth_1]
}
must "launch synth_1" {
    launch_runs synth_1 -jobs $jobs
    wait_on_run synth_1
}
check_run synth_1

# ============================================================================================
banner 6 "implementation (opt / place / phys_opt / route, Explore)"
# ============================================================================================
set impl [get_runs impl_1]
may "opt directive"          { set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE Explore $impl }
may "place directive"        { set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Explore $impl }
may "phys_opt enable"        { set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true $impl }
may "phys_opt directive"     { set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE Explore $impl }
may "route directive"        { set_property STEPS.ROUTE_DESIGN.ARGS.DIRECTIVE Explore $impl }
may "post-route phys_opt"    {
    set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true $impl
    set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE Explore $impl
}
must "launch impl_1" {
    launch_runs impl_1 -jobs $jobs
    wait_on_run impl_1
}
check_run impl_1

# ============================================================================================
banner 7 "reports"
# ============================================================================================
must "open_run impl_1" { open_run impl_1 }
must "timing summary" {
    report_timing_summary -delay_type min_max -max_paths 20 -report_unconstrained \
        -check_timing_verbose -input_pins -file [file join $out_dir timing_summary.rpt]
}
must "utilization" { report_utilization -file [file join $out_dir utilization.rpt] }
may  "hierarchical utilization" {
    report_utilization -hierarchical -file [file join $out_dir utilization_hier.rpt]
}
may "clocks"            { report_clocks -file [file join $out_dir clocks.rpt] }
may "clock interaction" { report_clock_interaction -delay_type min_max -file [file join $out_dir clock_interaction.rpt] }
may "drc"               { report_drc -file [file join $out_dir drc.rpt] }
may "methodology"       { report_methodology -file [file join $out_dir methodology.rpt] }
may "cdc"               { report_cdc -details -file [file join $out_dir cdc.rpt] }

# bus skew (async FIFO gray pointers, fpga/pz7020_gpu.xdc): report + parse. Vivado 2026.1 prints
# each summary row over 3 lines ("Id Position From" / "To" / "Corner Requirement Actual Slack")
# and per constraint an "Id: n" block with "Endpoints: k" (seen in a real run, fpga/build_try).
set bus_skew_txt ""
may "bus skew" {
    report_bus_skew -max_paths 4 -file [file join $out_dir bus_skew.rpt]
    set bus_skew_txt [report_bus_skew -max_paths 4 -return_string]
}
set bus_skew_rows {}       ;# {id corner requirement actual slack}
set bus_skew_endpoints {}  ;# {id endpoints}
set in_sum 0
set bs_id ""
foreach line [split $bus_skew_txt "\n"] {
    if {[string match "1. Bus Skew Report Summary*" $line]} { set in_sum 1; continue }
    if {[string match "2. Bus Skew Report Per Constraint*" $line]} { set in_sum 0; continue }
    if {$in_sum && [regexp {^\s*(\d+)\s+(\d+)\s} $line -> id pos]} { set bs_id $id }
    if {$in_sum && [regexp {^\s*(Slow|Fast)\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+|inf)\s*$} $line \
            -> corner req act slk]} {
        lappend bus_skew_rows [list $bs_id $corner $req $act $slk]
    }
    if {!$in_sum && [regexp {^Id:\s*(\d+)} $line -> id]} { set bs_id $id }
    if {!$in_sum && [regexp {^Endpoints:\s*(\d+)} $line -> n]} { lappend bus_skew_endpoints [list $bs_id $n] }
}
# every bit of each 11-bit pointer must be covered (a gray bit merged into another register by
# synthesis would silently drop out of the constraint)
set gray_bits [llength [get_cells -quiet {u_pl/u_scan/u_fifo/wgray_s1_reg[*]}]]
info_msg "bus skew: rows $bus_skew_rows, endpoints $bus_skew_endpoints, pointer width $gray_bits"

set wns ""; set whs ""; set tns ""; set ths ""
catch {set wns [get_property SLACK [get_timing_paths -delay_type max -max_paths 1 -nworst 1]]}
catch {set whs [get_property SLACK [get_timing_paths -delay_type min -max_paths 1 -nworst 1]]}
catch {set tns [get_property STATS.TNS $impl]}
catch {set ths [get_property STATS.THS $impl]}
info_msg "timing: WNS=$wns ns TNS=$tns ns WHS=$whs ns THS=$ths ns"

# worst setup / hold slack per clock (paths ending at that clock, intra- and inter-clock)
set per_clock {}
foreach c [lsort [get_clocks]] {
    set s ""; set h ""; set per ""
    catch {set per [get_property PERIOD $c]}
    catch {set s [get_property SLACK [get_timing_paths -to $c -delay_type max -max_paths 1 -nworst 1]]}
    catch {set h [get_property SLACK [get_timing_paths -to $c -delay_type min -max_paths 1 -nworst 1]]}
    if {$s eq ""} { set s "-" }
    if {$h eq ""} { set h "-" }
    set line [format "%-12s period %7s ns   WNS %7s ns   WHS %7s ns" $c $per $s $h]
    lappend per_clock $line
    info_msg "clock $line"
}

# utilization headline numbers
set util_lines {}
set util_txt ""
catch {set util_txt [report_utilization -return_string]}
foreach key {{Slice LUTs} {Slice Registers} {Block RAM Tile} {DSPs} {Bonded IOB} {BUFGCTRL} {MMCME2_ADV}} {
    # table row: | <key> | Used | Fixed | Prohibited | Available | Util% |
    set re [string map [list KEY $key] {^\|\s*KEY\*?\s*\|\s*([0-9.]+)\s*\|.*\|\s*([0-9.]+)\s*\|\s*([0-9.<]+)\s*\|\s*$}]
    foreach line [split $util_txt "\n"] {
        if {[regexp $re $line -> used avail pct]} {
            lappend util_lines [format "%-16s %8s / %-8s (%s %%)" $key $used $avail $pct]
            break
        }
    }
}
foreach u $util_lines { info_msg "utilization: $u" }

# critical warnings: this session (block design, constraints, reports) + the synth/impl runs
set cw_session 0
catch {set cw_session [get_msg_config -count -severity {CRITICAL WARNING}]}
set cw_lines {}
foreach run {synth_1 impl_1} {
    set lf [file join [get_property DIRECTORY [get_runs $run]] runme.log]
    if {![file exists $lf]} { continue }
    set fh [open $lf r]
    foreach l [split [read $fh] "\n"] {
        if {[string match "CRITICAL WARNING*" $l]} { lappend cw_lines "$run: $l" }
    }
    close $fh
}
set cw_lines [lsort -unique $cw_lines]
info_msg "critical warnings: $cw_session in this session, [llength $cw_lines] distinct in the synth/impl runs"
foreach l $cw_lines { puts "   $l" }

# ============================================================================================
banner 8 "bitstream -> [file join $out_dir pl.bit]"
# ============================================================================================
must "write_bitstream" {
    write_bitstream -force -bin_file [file join $out_dir pl.bit]
}
if {![file exists [file join $out_dir pl.bit]]} { die "write_bitstream did not produce pl.bit" }
info_msg "wrote [file join $out_dir pl.bit] ([file size [file join $out_dir pl.bit]] bytes)"

# ============================================================================================
banner 9 "timing verdict"
# ============================================================================================
set failed 0
set why {}
if {![string is double -strict $wns]} {
    set failed 1
    lappend why "no setup timing paths found (WNS '$wns')"
} elseif {$wns < 0} {
    set failed 1
    lappend why "setup WNS = $wns ns"
}
if {![string is double -strict $whs]} {
    set failed 1
    lappend why "no hold timing paths found (WHS '$whs')"
} elseif {$whs < 0} {
    set failed 1
    lappend why "hold WHS = $whs ns"
}
# the XDC declares 2 set_bus_skew constraints (write and read gray pointer of the scanout FIFO)
set bs_ids {}
foreach r $bus_skew_rows { lappend bs_ids [lindex $r 0] }
set bs_ids [lsort -unique $bs_ids]
if {[llength $bs_ids] != 2} {
    set failed 1
    lappend why "expected 2 bus-skew constraints in report_bus_skew, found [llength $bs_ids] (see out/bus_skew.rpt)"
}
foreach r $bus_skew_rows {
    lassign $r id corner req act slk
    if {![string is double -strict $slk] || $slk < 0} {
        set failed 1
        lappend why "bus skew constraint $id ($corner): skew $act ns, requirement $req ns, slack $slk"
    }
}
if {[llength $bus_skew_endpoints] != 2} {
    set failed 1
    lappend why "bus skew: expected 2 'Endpoints:' entries, found [llength $bus_skew_endpoints]"
}
foreach e $bus_skew_endpoints {
    lassign $e id n
    if {$gray_bits == 0 || $n != $gray_bits} {
        set failed 1
        lappend why "bus skew constraint $id covers $n endpoints, the gray pointer has $gray_bits bits"
    }
}

set fh [open [file join $out_dir build_summary.txt] w]
puts $fh "FPGA-GPU build [clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S}]"
puts $fh "Vivado [version -short], part $part, top $top_name, core clock $core_hz Hz"
puts $fh "WNS $wns ns, TNS $tns ns, WHS $whs ns, THS $ths ns"
puts $fh "per clock (worst path ending at the clock):"
foreach l $per_clock { puts $fh "  $l" }
puts $fh "bus skew (async FIFO gray pointers; requirement / actual skew / slack ns, endpoints):"
foreach r $bus_skew_rows {
    lassign $r id corner req act slk
    set n "?"
    foreach e $bus_skew_endpoints { if {[lindex $e 0] eq $id} { set n [lindex $e 1] } }
    puts $fh "  #$id $corner: $req / $act / $slk   endpoints $n of $gray_bits bits"
}
puts $fh "utilization:"
foreach u $util_lines { puts $fh "  $u" }
puts $fh "critical warnings: $cw_session in the build session, [llength $cw_lines] distinct in synth/impl runs"
foreach l $cw_lines { puts $fh "  $l" }
puts $fh "timing [expr {$failed ? {NOT MET} : {met}}]"
foreach w $why { puts $fh "  $w" }
close $fh

if {$failed} {
    set fh [open [file join $out_dir TIMING_FAILED.txt] w]
    puts $fh "TIMING NOT MET -- pl.bit was written anyway but may not work reliably."
    foreach w $why { puts $fh $w }
    puts $fh "See out/timing_summary.rpt and out/bus_skew.rpt."
    close $fh
    puts ""
    puts "############################################################################"
    puts "### \[build\] TIMING NOT MET: [join $why {; }]"
    puts "### pl.bit written anyway; see out/TIMING_FAILED.txt and out/timing_summary.rpt"
    puts "############################################################################"
    flush stdout
    exit 2
}
if {$cw_session > 0 || [llength $cw_lines] > 0} {
    puts ""
    puts "=== \[build\] DONE WITH CRITICAL WARNINGS: timing met (WNS $wns ns, WHS $whs ns), out/pl.bit written;"
    puts "=== $cw_session critical warning(s) in the session, [llength $cw_lines] in the runs -- see out/build_summary.txt ==="
    flush stdout
    exit 3
}
puts ""
puts "=== \[build\] DONE: timing met (WNS $wns ns, WHS $whs ns), no critical warnings, out/pl.bit written ==="
flush stdout
exit 0
