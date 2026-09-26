# core_ooc.xdc -- out-of-context timing constraint for core_top (SPEC 2: core clock 148.75 MHz)
create_clock -period 6.723 -name clk [get_ports clk]
# route the OOC clock from a real global buffer site so clock skew is modelled
set_property HD.CLK_SRC BUFGCTRL_X0Y16 [get_ports clk]
