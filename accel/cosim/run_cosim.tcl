# run_cosim.tcl -- xsim: the real AXI DMA IP + zaccel_gemv + gemv_reset, driven by tb_cosim.v.
#   python gen_cosim.py && vivado -mode batch -source run_cosim.tcl        (from accel/cosim)
set here [file dirname [file normalize [info script]]]
set repo [file normalize $here/../..]
if {![file exists $here/ip/dma0/dma0.xci]} { source $here/make_ip.tcl }
create_project cosim $here/prj -part xc7z020clg400-2 -force
set_property target_language Verilog [current_project]
read_ip $here/ip/dma0/dma0.xci
generate_target simulation [get_ips dma0]
add_files -fileset sim_1 -norecurse [list $here/tb_cosim.v $repo/accel/rtl/zaccel_gemv.v \
    $repo/hardware/pz7020-starlite/vivado/gemv_reset.v]
set_property include_dirs [list $here] [get_filesets sim_1]
set_property top tb [get_filesets sim_1]
set_property -name {xsim.simulate.runtime} -value {all} -objects [get_filesets sim_1]
launch_simulation -scripts_only
puts "COSIM RUN FINISHED"
close_project
