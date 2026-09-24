# PZ7020-StarLite without Vivado — the open toolchain, measured

Vivado is not on this PC and needs an AMD account to download. The Zynq-7000's PL is fully
covered by the open flow (Project X-Ray database + nextpnr-xilinx), and the PS can be brought up
with U-Boot SPL using the hardware-validated `ps7_init_gpl.c` in [ps7/](ps7/). Everything on
this page was **run on this PC on 2026-09-24**; the numbers are from the tools' own reports.

## 1. Toolchain (installed, verified)

| Piece | Where | Version / proof |
|---|---|---|
| yosys | `D:\espicpc\tools\oss-cad-suite\bin` | 0.69+20 (`synth_xilinx`) |
| nextpnr-xilinx, fasm2frames, xc7frames2bit, bitread | `C:\Users\Danie\.apio\packages\openxc7\bin` | openXC7 build 2026-09-15 via `pip install apio; apio packages install openxc7` |
| Project X-Ray DB for **xc7z020clg400-2** | `…\openxc7\share\nextpnr\external\prjxray-db\zynq7\` | shipped in the package |
| Chip database `xc7z020clg400.bin` | `…\openxc7\chipdb\` (136,617,100 B) | asset + file SHA-256 both match `XILINX-PARTS-INDEX.json` |
| openFPGALoader 1.1.1 | oss-cad-suite | cables for FT232H (0403:6014) present |
| iverilog / vvp | oss-cad-suite (`lib` must be on PATH for vvp) | all 5 SDR testbenches PASS |

Build script: [`firmware/rtlsdr-pentest/fpga/openxc7/build.sh`](../../firmware/rtlsdr-pentest/fpga/openxc7/build.sh)
— `build.sh <top> <out> <full.xdc> <verilog…>`. It strips the XDC down to the pin/IO lines nextpnr
understands (`PACKAGE_PIN, IOSTANDARD, PULLUP/PULLDOWN, DRIVE, SLEW`); clocks are given as
`--freq`. The full XDC stays the source of truth for Vivado.

## 2. First real numbers for this project on the actual part

| Design | LUT | FF | DSP48 | RAMB18 | Fmax @50 MHz target | Bitstream |
|---|---|---|---|---|---|---|
| `fan_top` (LED heartbeat + fan PWM/tach, [fan/](fan/)) | 174 / 106,400 | 71 | 0 | 0 | **242.8 MHz** | `fan/build/fan_top.bit` 4,045,675 B |
| `sdr_accel_zynq_top` (256-pt FFT over SPI, RTL-SDR node) | 1,056 (1.0 %) | 445 | **4 / 220** | **7 / 280** | **82.4 MHz** | `openxc7/build/sdr_accel_zynq_top.bit` 4,045,734 B |

What this settles: the SDR accelerator that was only ever simulated fits in 1 % of the fabric,
uses 4 DSP slices, and closes at 50 MHz with 65 % margin — 📐 estimates in FPGA-CAPABILITY.md
are now superseded by these for that block. Both bitstreams are the standard 4.04 MB xc7z020
size, produced by xc7frames2bit from 7,802 frame lines each (full, non-sparse).

Not yet measured: the audio front-end (no RTL yet) and the bench-one int4 lanes on this flow.

## 3. Programming the PL over the on-board USB-JTAG

Schematic sheet 19: the JTAG bridge is an **FTDI FT232H (U17)** with a **93LC56B EEPROM (U18)**
on the "PWR+JTAG" Type-C — that EEPROM is what lets Vivado recognise it as a Digilent-class
cable. The USB VID:PID is therefore **0403:6014**. With the board on the PWR+JTAG port:

```bash
export PATH="/d/espicpc/tools/oss-cad-suite/bin:/d/espicpc/tools/oss-cad-suite/lib:$PATH"
openFPGALoader --detect -c digilent_hs2                 # expect: xc7z020 (idcode 0x?3727093)
openFPGALoader -c digilent_hs2 --fpga-part xc7z020clg400 hardware/pz7020-starlite/fan/build/fan_top.bit
```

If `--detect` sees nothing with `digilent_hs2`, the EEPROM pin mapping differs — try `-c ft232`
(plain MPSSE on ADBUS0-3), then `-c digilent_ad` / `-c jtag-smt2-nc`. Expected result on the board:
**LED1 (R19) blinks ~1 Hz** and the fan PWM appears on JM1 pin 5. This is a PL-only load with the
boot jumper on JTAG; power-cycle clears it. **Not run yet — the board was not attached when this
was built (no 0403/1a86 USB device present).**

## 4. Simulation vs synthesis — the regression that still stands

The `.bit` files are built from exactly the RTL the testbenches verified bit-exact
(`tb_sdr_accel_spi`: all 256 bins match the Python model, peak bin 236). Any RTL change re-runs:

```bash
cd firmware/rtlsdr-pentest/fpga/rtl && export PATH="/d/espicpc/tools/oss-cad-suite/bin:/d/espicpc/tools/oss-cad-suite/lib:$PATH"
for tb in tb_spi_loopback tb_sdr_accel_spi tb_sdr_fft_top tb_fft_engine tb_ddc; do iverilog -g2012 -o /tmp/$tb.vvp ../tb/$tb.v *.v && vvp /tmp/$tb.vvp | grep -E "PASS|FAIL"; done
```

## 5. What the open flow does not give you

- No PS7 block design, no XSA: the PS is configured by `ps7_init_gpl.c` in U-Boot SPL instead
  (see [PS-LINUX.md](PS-LINUX.md)). Vivado-generated AXI IP (GMII-to-RGMII, AXI DMA…) needs Vivado;
  open equivalents exist (verilog-ethernet, custom AXI-Lite) but are not built here yet.
- Timing analysis is nextpnr's, not Vivado's: good for "does it close", not sign-off.
- Docker Desktop on this PC never brought its engine up (pipe missing after 5 min); the Linux
  build host is a fresh **WSL2 Ubuntu-22.04** instead.
