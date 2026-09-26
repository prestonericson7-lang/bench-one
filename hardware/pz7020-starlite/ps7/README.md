# ps7/ -- hardware-validated PS initialisation for the PZ7020-StarLite

Source: `Hiroto-Nakano/PZ7020StarLite` (MIT, 2026), a working Vivado 2024.1 + PetaLinux 2024.1
build for this exact board (XC7Z020CLG400-2, boots Linux from SD, LED demo on hardware).
The files here are lifted from its exported hardware platform `PZ7020StarLite_wrapper.xsa`:

| File | What it is | Licence |
|---|---|---|
| `ps7_init_gpl.c` / `.h` | The Zynq PS register initialisation (clocks, MIO, DDR PHY + controller, training) that Vivado generated for this board. This is what U-Boot SPL compiles in as the FSBL replacement. | GPL-2.0+ (AMD ships it under GPL precisely for U-Boot) |
| `ps7_init.tcl` | Same init sequence for XSCT/JTAG bring-up (`source ps7_init.tcl; ps7_init; ps7_post_config`). | as distributed in every XSA |
| `ps7_parameters_validated.json` | Every non-derived `CONFIG.PCW_*` of the working block design's `processing_system7` (v5.5). | -- |

**What the working design settles** (its docs/WORKFLOW.md, confirmed by the parameter dump):

- **DDR: this set runs the bus 16-bit (512 MB visible)**, `DDR 3 (Low Voltage)`, part `MT41K256M16 RE-125`;
  plain `DDR 3` (1.5 V) fails `DDR_INIT_FAIL`. **This is THE configuration:** the board has one x16
  MT41K256M16 (512 MB; schematic wires DQ0-15 and A0-A14 only). `../ps7-vivado/` (32-bit / 1 GB) was
  silent on the board on 2026-09-26 and `linux/build_uboot.sh` refuses it.
- APU 766.67 MHz (ARM PLL FDIV=46 from the 33.333 MHz crystal); FCLK0 100 MHz; QSPI MIO1-6 at
  200 MHz x4; UART0 MIO10/11; GEM0 MIO16-27 + MDIO 52/53 (PHY addr 1, Realtek, DT props
  `clkout-disable`, `aldps-enable`); SD0 MIO40-45 with no CD/WP; USB0 MIO28-39.
- Their design does not enable GEM1/EMIO, USB reset on MIO46, or S_AXI_HP; ours adds those
  (`../constraints/ps7_starlite.tcl`) on top of the same validated base.
