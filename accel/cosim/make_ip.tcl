# make_ip.tcl -- the AXI DMA exactly as build_system.tcl configures it, as a standalone IP for simulation.
#   vivado -mode batch -source make_ip.tcl      (from accel/cosim)
set here [file dirname [file normalize [info script]]]
create_project -in_memory -part xc7z020clg400-2
set_property target_language Verilog [current_project]
file mkdir $here/ip
create_ip -name axi_dma -vendor xilinx.com -library ip -module_name dma0 -dir $here/ip
set_property -dict [list CONFIG.c_include_sg 0 CONFIG.c_sg_length_width 26 CONFIG.c_addr_width 32 \
    CONFIG.c_include_mm2s 1 CONFIG.c_include_s2mm 1 \
    CONFIG.c_m_axi_mm2s_data_width 64 CONFIG.c_m_axis_mm2s_tdata_width 64 CONFIG.c_mm2s_burst_size 16 \
    CONFIG.c_m_axi_s2mm_data_width 64 CONFIG.c_s_axis_s2mm_tdata_width 64 CONFIG.c_s2mm_burst_size 16] [get_ips dma0]
generate_target {instantiation_template simulation} [get_ips dma0]
export_ip_user_files -of_objects [get_ips dma0] -no_script -force
puts "IP: [get_files -of [get_ips dma0]]"
puts "DONE"
