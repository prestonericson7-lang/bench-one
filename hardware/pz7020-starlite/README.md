# PZ7020-StarLite (Puzhi) — board reference, pinout & fan wiring

Plain-English reference for the **Puzhi PZ7020-StarLite** Zynq-7000 board, its complete
expansion-header pinout, and how to wire a fan. Sourced from the official doc bundle:
**User Manual**, **`Puzhi PZ-StarLite Schematic.pdf`**, and the authoritative
**`CON Pins Signal and Equal Length.xlsx`** (04.Hardware). Written so the next person
doesn't re-derive any of it.

---

## 1. Board at a glance

| Item | Value |
|------|-------|
| Board | PZ7020-StarLite (variant PZ7020-SL-C), Puzhi |
| SoC | Xilinx **Zynq XC7Z020-CLG400** — dual Cortex-A9 PS @766 MHz + Artix-7 PL |
| PL fabric | 85K logic cells, 53,200 LUT, 106,400 FF, **220 DSP48**, ~4.9 Mb BRAM |
| DDR3 | Micron **MT41K256M16TW-107** (4 Gb ×16). **Schematic V1.0 shows ONE chip on a 16-bit bus = 512 MB**; the manual's table lists 1 GB for the 7020, which needs two chips / 32-bit. **Count the DRAM packages on the board** before configuring the PS — see [PS-CONFIG.md](PS-CONFIG.md) §1 |
| Boot / storage | 128 Mb QSPI (W25Q128JV), 64 Kbit E²PROM (AT24C64, I²C BANK91), microSD (BANK501, 1.8 V) |
| Clocks | PS 33.333 MHz (PS_REF_CLK); **PL 50 MHz single-ended = IO_12P_MRCC_34, ball `U18`** |
| Networking | 2× Gigabit Ethernet (RTL8211FD; 1 PS-side, 1 PL-side) |
| Video/other | HDMI out (BANK34), MIPI CSI 2-lane, USB 2.0 host (BANK501, 1.8 V) |
| Power | **5 V / 1 A** — via Type-C **or** the 40P header 5 V pins |
| Reset | nRST (active low) → PS `PS_POR_B (C7)` + PL `IO_L12N_MRCC_34 (U19)` |
| LEDs | LED1 `R19`, LED2 `V13` (BANK34, high = on) |
| KEYs | KEY1 `G14`, KEY2 `J15` (BANK35, low = pressed) |
| Toolchain | **AMD/Xilinx Vivado** (repo's yosys/nextpnr are iCE40/ECP5 only) |

---

## 2. Serial console, boot, and the "is it alive?" test

- **USB-UART = CH340E** → enumerates as a **CH340 COM port** (was COM31), *not* FT2232. Wired to the **PS side**: `UART_TX=MIO11 (C6)`, `UART_RX=MIO10 (E9)`, 3.3 V.
- **The UART only transmits when the PS boots code.** Boot jumper on **JTAG with nothing loaded → PS halts → line is silent** (measured silent at all bauds). That is correct, not a fault. To get console output: boot from **SD** (bootable image) or **QSPI**, then read the CH340 port at **115200 8N1**.
- **Factory self-test (zero risk):** a **LED-blink demo is pre-burned into QSPI Flash** (per `03.Boot Test/readme.txt`). Set the boot jumper to **QSPI**, power on → **the two user LEDs blink** = the board and PL are alive. This is the safe way to confirm the board works without touching anything.

---

## 3. Expansion headers JM1 / JM2 — full pinout

- 40-pin, 2.54 mm, **"optional solder"** (may need the 2×20 headers soldered on).
- Level per bank: **HR BANK, 1.8 / 2.5 / 3.3 V adjustable, default 3.3 V** (set by resistor position). → a fan PWM at **3.3 V LVCMOS33** works directly.
- **JM1 is entirely BANK35. JM2 is mixed: pins 5–20 = BANK35, pins 21–40 = BANK34.**
- Balls below are the FPGA `PACKAGE_PIN` (from the equal-length xlsx — authoritative).

### JM1 (all BANK35)
| Pin | Signal | Ball | | Pin | Signal | Ball |
|:---:|:-------|:----:|-|:---:|:-------|:----:|
| **1** | **5 V** | — | | **2** | **3.3 V** | — |
| **3** | **GND** | — | | **4** | **GND** | — |
| 5 | IO_13P_35 | H16 | | 6 | IO_3P_35 | E17 |
| 7 | IO_13N_35 | H17 | | 8 | IO_3N_35 | D18 |
| 9 | IO_5P_35 | E18 | | 10 | IO_6P_35 | F16 |
| 11 | IO_5N_35 | E19 | | 12 | IO_6N_35 | F17 |
| 13 | IO_16P_35 | G17 | | 14 | IO_2P_35 | B19 |
| 15 | IO_16N_35 | G18 | | 16 | IO_2N_35 | A20 |
| 17 | IO_4P_35 | D19 | | 18 | IO_1P_35 | C20 |
| 19 | IO_4N_35 | D20 | | 20 | IO_1N_35 | B20 |
| 21 | IO_14P_35 | J18 | | 22 | IO_10P_35 | K19 |
| 23 | IO_14N_35 | H18 | | 24 | IO_10N_35 | J19 |
| 25 | IO_12P_35 | K17 | | 26 | IO_8P_35 | M17 |
| 27 | IO_12N_35 | K18 | | 28 | IO_8N_35 | M18 |
| 29 | IO_11P_35 | L16 | | 30 | IO_15P_35 | F19 |
| 31 | IO_11N_35 | L17 | | 32 | IO_15N_35 | F20 |
| **33** | **GND** | — | | **34** | **GND** | — |
| **35** | **GND** | — | | **36** | **GND** | — |
| 37 | IO_7P_35 | M19 | | 38 | IO_9P_35 | L19 |
| 39 | IO_7N_35 | M20 | | 40 | IO_9N_35 | L20 |

### JM2 (pins 5–20 BANK35, pins 21–40 BANK34)
| Pin | Signal | Ball | | Pin | Signal | Ball |
|:---:|:-------|:----:|-|:---:|:-------|:----:|
| **1** | **5 V** | — | | **2** | **3.3 V** | — |
| **3** | **GND** | — | | **4** | **GND** | — |
| 5 | IO_18P_35 | G19 | | 6 | IO_17P_35 | J20 |
| 7 | IO_18N_35 | G20 | | 8 | IO_17N_35 | H20 |
| 9 | IO_19P_35 | H15 | | 10 | IO_20P_35 | K14 |
| 11 | IO_19N_35 | G15 | | 12 | IO_20N_35 | J14 |
| 13 | IO_24P_35 | K16 | | 14 | IO_22P_35 | L14 |
| 15 | IO_24N_35 | J16 | | 16 | IO_22N_35 | L15 |
| 17 | IO_21P_35 | N15 | | 18 | IO_23P_35 | M14 |
| 19 | IO_21N_35 | N16 | | 20 | IO_23N_35 | M15 |
| 21 | IO_9P_34 | T16 | | 22 | IO_5P_34 | T14 |
| 23 | IO_9N_34 | U17 | | 24 | IO_5N_34 | T15 |
| 25 | IO_6P_34 | P14 | | 26 | IO_2P_34 | T12 |
| 27 | IO_6N_34 | R14 | | 28 | IO_2N_34 | U12 |
| 29 | IO_1P_34 | T11 | | 30 | IO_7P_34 | Y16 |
| 31 | IO_1N_34 | T10 | | 32 | IO_7N_34 | Y17 |
| **33** | **GND** | — | | **34** | **GND** | — |
| **35** | **GND** | — | | **36** | **GND** | — |
| 37 | IO_4P_34 | V12 | | 38 | IO_8P_34 | W14 |
| 39 | IO_4N_34 | W13 | | 40 | IO_8N_34 | Y14 |

*(Signal-to-ball wire lengths are in the xlsx if you ever need length-matching for high-speed diff pairs.)*

---

## 4. Wiring a fan — fully specified

**Never power a fan from an FPGA I/O pin** — an I/O sources a few mA; a fan pulls 100–500 mA. The FPGA supplies only the control signal.

### Exact connections (JM1, all confirmed from the pinout table)
| Fan wire | JM1 pin | FPGA ball | Notes |
|----------|:-------:|:---------:|-------|
| **PWM** (control) | **pin 5** = IO_13P_35 | **H16** | 3.3 V LVCMOS33, 25 kHz; add 10 kΩ pull-down |
| **TACH** (RPM) | **pin 7** = IO_13N_35 | **H17** | LVCMOS33 + internal PULLUP |
| **+5 V** | **pin 1** (5 V) | — | 5 V fan only; a 12 V fan needs an external 12 V supply, GND common |
| **GND** | **pin 3** (GND) | — | shared ground |

- PWM (pin 5) and TACH (pin 7) are the two halves of the IO_13 pair — adjacent on the header and right next to the 5 V (pin 1) / GND (pin 3) power pins. Convenient wiring.
- **4-wire fan:** all four wires as above, direct.
- **2/3-wire fan:** the FPGA can't switch motor current — add an **N-channel logic-level MOSFET** (gate ← H16 via 100 Ω + 10 kΩ pull-down; drain → fan−; source → GND; fan+ → 5 V/12 V).

### XDC (real balls — ready to use)
```tcl
## Fan control — PZ7020-StarLite JM1, BANK35 @ 3.3 V
set_property PACKAGE_PIN H16      [get_ports fan_pwm]    ;# JM1 pin 5
set_property IOSTANDARD  LVCMOS33 [get_ports fan_pwm]

set_property PACKAGE_PIN H17      [get_ports fan_tach]   ;# JM1 pin 7
set_property IOSTANDARD  LVCMOS33 [get_ports fan_tach]
set_property PULLUP      true     [get_ports fan_tach]
```
PL PWM off the 50 MHz clock (ball U18): period for 25 kHz = 50e6 / 25e3 = **2000 ticks**; duty = compare 0–2000. **Pin assignment is the FPGA perk** — bind the PWM to any free 3.3 V pin in the tables above; H16/H17 is just a clean, power-adjacent choice.

---

## 5. FPGA + Orange Pi 4 Pro — what the Zynq adds

> Superseded in detail by [SYSTEM-INTEGRATION.md](SYSTEM-INTEGRATION.md) (pin-level links, bring-up order,
> corrections) and [PS-CONFIG.md](PS-CONFIG.md) (PS7 settings from the schematic). Master constraints:
> [constraints/pz7020_starlite_board.xdc](constraints/pz7020_starlite_board.xdc).

The Orange Pi 4 Pro (Allwinner **A733**: 2× A76 + 6× A55 + RISC-V, up to 16 GB LPDDR5,
GPU **+ NPU**) already has strong graphics/AI, so the Zynq is **not** a "GPU." Its value is
what a Linux SBC does badly: **real-time RF/DSP** (FFT/DDC/filter on 220 DSP48 at line rate),
**hard-real-time & custom I/O** (ns triggers, protocol bit-bang, logic analysis), and
**parallel bit-level compute** (the "matrix engine" role).

**Linking them (highest → simplest BW):** M.2 PCIe 3.0 (heavy, needs a PL endpoint) →
**Gigabit Ethernet** (pragmatic bulk path; Zynq has 2×) → **SPI/UART over JM1/JM2** (control
+ modest data) → MIPI CSI (feed processed data in as a "camera").

---

## 6. Sources
- **ZYNQ7000 PZ-StarLite FPGA Board User Manual** — Parts 3.2/3.4/3.7/3.10/3.16/3.18.
- **04.Hardware/** doc bundle: `Puzhi PZ-StarLite Schematic.pdf`, `CON Pins Signal and Equal Length.xlsx` (authoritative pinout), `03.Boot Test/readme.txt` (QSPI LED demo).
- Component datasheets (01.Datasheet): DDR3 MT41K256M16TW-107, W25Q128JV, RTL8211FD, CH340E, AT24C64D, Xilinx 7-series UGs.
- Orange Pi 4 Pro / Allwinner A733 — https://www.cnx-software.com/2025/10/25/35-orange-pi-4-pro-an-allwinner-a733-edge-ai-sbc-with-up-to-16gb-lpddr5-wifi-6/

*Seller doc/schematic contact: support@aithtech.com (Dropbox link + password).*
