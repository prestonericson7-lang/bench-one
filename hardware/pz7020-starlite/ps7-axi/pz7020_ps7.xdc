# pz7020_ps7.xdc -- pins for pz7020_ps7_top on the PZ7020-StarLite.
# Balls from the vendor manual (LEDs Part 3.16, KEYs 3.17) and the CON pins xlsx (JM1) -- see
# ../constraints/pz7020_starlite_board.xdc for the full map. No external clock: FCLK0 comes from the PS.
set_property CFGBVS VCCO        [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]

set_property PACKAGE_PIN R19      [get_ports led1]      ;# LED1  IO_0_34
set_property IOSTANDARD  LVCMOS33 [get_ports led1]
set_property PACKAGE_PIN V13      [get_ports led2]      ;# LED2  IO_L3N_34
set_property IOSTANDARD  LVCMOS33 [get_ports led2]

set_property PACKAGE_PIN G14      [get_ports key1_n]    ;# KEY1  IO_0_35
set_property IOSTANDARD  LVCMOS33 [get_ports key1_n]
set_property PULLUP      true     [get_ports key1_n]
set_property PACKAGE_PIN J15      [get_ports key2_n]    ;# KEY2  IO_25_35
set_property IOSTANDARD  LVCMOS33 [get_ports key2_n]
set_property PULLUP      true     [get_ports key2_n]

set_property PACKAGE_PIN H16      [get_ports fan_pwm]   ;# JM1 pin 5  IO_13P_35
set_property IOSTANDARD  LVCMOS33 [get_ports fan_pwm]
set_property PACKAGE_PIN H17      [get_ports fan_tach]  ;# JM1 pin 7  IO_13N_35
set_property IOSTANDARD  LVCMOS33 [get_ports fan_tach]
set_property PULLUP      true     [get_ports fan_tach]
