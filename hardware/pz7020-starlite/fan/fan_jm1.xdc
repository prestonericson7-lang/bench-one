#-----------------------------------------------------------------------------
# fan_jm1.xdc -- PZ7020-StarLite constraints for hdl/fan_top.v
#
# Board : Puzhi PZ7020-StarLite, Zynq XC7Z020-CLG400
# Part  : xc7z020clg400-2        (manual states XC7Z020-2CLG400I)
#
# Ball assignments are from Puzhi's "CON Pins Signal and Equal Length.xlsx"
# and the User Manual (Parts 3.2, 3.16, 3.17, 3.18).
# See docs/pinout.md for the complete header map.
#
# JM1 is entirely on BANK35 (HR bank, 1.8/2.5/3.3 V selectable, DEFAULT 3.3 V),
# so LVCMOS33 is correct unless you have changed the bank resistor option.
#-----------------------------------------------------------------------------

## ---------------------------------------------------------------------------
## Fabric clock -- 50 MHz single-ended, IO_12P_MRCC_34
## ---------------------------------------------------------------------------
set_property PACKAGE_PIN U18      [get_ports clk_50m]
set_property IOSTANDARD  LVCMOS33 [get_ports clk_50m]
create_clock -period 20.000 -name clk_50m -waveform {0.000 10.000} [get_ports clk_50m]

## ---------------------------------------------------------------------------
## User LED1 -- heartbeat (BANK34). High = lit.
## ---------------------------------------------------------------------------
set_property PACKAGE_PIN R19      [get_ports led_hb]
set_property IOSTANDARD  LVCMOS33 [get_ports led_hb]

## ---------------------------------------------------------------------------
## User KEY1 -- active low (BANK35). Held = fan to 100%.
## ---------------------------------------------------------------------------
set_property PACKAGE_PIN G14      [get_ports key_n]
set_property IOSTANDARD  LVCMOS33 [get_ports key_n]
set_property PULLUP      true     [get_ports key_n]

## ---------------------------------------------------------------------------
## Fan control -- JM1
##   fan_pwm  : JM1 pin 5, IO_13P_35, ball H16   (output to fan PWM wire)
##   fan_tach : JM1 pin 7, IO_13N_35, ball H17   (input from fan TACH wire)
##
## Fan power does NOT come from these pins:
##   fan +5V  -> JM1 pin 1 (5 V)     [5 V fans only; 12 V fans need an external supply]
##   fan GND  -> JM1 pin 3 (GND)     [must be common with the fan's supply ground]
## ---------------------------------------------------------------------------
set_property PACKAGE_PIN H16      [get_ports fan_pwm]
set_property IOSTANDARD  LVCMOS33 [get_ports fan_pwm]
set_property DRIVE       8        [get_ports fan_pwm]
set_property SLEW        SLOW     [get_ports fan_pwm]

set_property PACKAGE_PIN H17      [get_ports fan_tach]
set_property IOSTANDARD  LVCMOS33 [get_ports fan_tach]
set_property PULLUP      true     [get_ports fan_tach]

## The tach input is asynchronous and is synchronised in RTL (two flops in
## fan_pwm.v), so exclude it from timing analysis.
set_false_path -from [get_ports fan_tach]
