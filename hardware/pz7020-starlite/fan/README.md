# fan/ -- first-bitstream design for the PZ7020-StarLite

Vendored from the owner's board repo `prestonericson7-lang/pz7020-starlite` (MIT): `fan_top.v` (LED1 heartbeat on R19, 25 kHz fan PWM on JM1 pin 5 = H16, tach on pin 7 = H17, KEY1 G14 -> 100%), `fan_pwm.v`, `fan_jm1.xdc`.
PL-only, so it runs with the boot jumper on JTAG and nothing in QSPI/SD. Built here with the open toolchain: `../../../firmware/rtlsdr-pentest/fpga/openxc7/build.sh`.
