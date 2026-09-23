# PZ7020-StarLite (Puzhi) — board reference & fan wiring

Plain-English reference for the **Puzhi PZ7020-StarLite** Zynq-7000 board, and how to
wire a fan to it. Written so the next person doesn't have to re-derive any of this.

> **Legend for every fact below**
> - ✅ **VERIFIED** — from Puzhi's own 2025 product guide or the SoC datasheet (sourced at the bottom).
> - ⚠️ **VERIFY ON BOARD** — likely true but must be checked against *your* board's silkscreen or the Puzhi board file before you trust it with hardware.
> - ⛔ **NOT PUBLIC** — only in the Puzhi doc bundle that ships with the board (Google Drive/Yandex/Dropbox link from their customer service).

---

## 1. What this board is

| Item | Value | Status |
|------|-------|--------|
| Board | PZ7020-StarLite, by Puzhi (SCFPGA / PuZhi) | ✅ |
| SoC | Xilinx **Zynq XC7Z020-CLG400** (dual Cortex-A9 @ 766 MHz PS + Artix-7 PL) | ✅ |
| PL fabric | 85K logic cells, 53,200 LUT, 106,400 FF, **220 DSP48**, ~4.9 Mb BRAM | ✅ |
| DDR3 (on carrier) | **1 GB** | ✅ confirmed on the physical board by the owner (PZ7020-SL-C variant; Puzhi's generic 2025 guide lists 512 MB/16-bit for the base StarLite, so this variant is upgraded) |
| Boot / storage | 128 Mb QSPI flash, 64 Kbit E²PROM, microSD slot | ✅ |
| Networking | **2× Gigabit Ethernet** (one PS-side, one PL-side) | ✅ |
| Other I/O | USB 2.0 host, HDMI out, MIPI CSI (2-lane), JTAG+UART over Type-C | ✅ |
| Power | **5 V / 1 A** via Type-C (that port is power **and** JTAG) | ✅ |
| Controls | 2 user LEDs, 2 user KEYs, NRST, boot-mode jumper (JTAG/QSPI/SD) | ✅ |
| Size | 90 × 60 mm | ✅ |
| Toolchain | **AMD/Xilinx Vivado** (not the repo's yosys/nextpnr — those are iCE40/ECP5 only) | ✅ |

---

## 2. The two 40-pin headers (JM1 / JM2)

- ✅ **Both are PL-side I/O breakout.** Together they expose **64 single-ended signals (= 32 differential pairs)**, plus power and ground.
- ✅ They are **"optional solder"** — the headers may not be populated on your board. Check; you may need to solder the 2×20 connectors on.
- ✅ **PL bank voltage is adjustable 1.8 / 2.5 / 3.3 V, default 3.3 V.** For a fan PWM you want a pin on a bank set to **3.3 V (LVCMOS33)**.
- ⚠️ **Power/ground pins per header:** board photos indicate each header carries **5 V, 3.3 V, and ~6 GND** among its 40 positions. Confirm exact positions on the silkscreen — do **not** assume.
- ⛔ **Exact header-pin → FPGA-ball (`PACKAGE_PIN`) map:** not published anywhere online (searched). It is in the Puzhi doc bundle.

### How to get the real pin map (do this once, then fill §5)
1. Get the doc bundle: email **support@aithtech.com** (the address on the card shipped with this board), quote your order — they reply with a **Dropbox link + password**.
2. In it, open the **example `.xdc` constraints file** (or the schematic). Every line like
   `set_property PACKAGE_PIN <ball> [get_ports <net>]` maps one header net to one FPGA ball.
3. Copy those into the table in §5. That table then *is* your pinout sheet.

---

## 3. Wiring a fan — the rules

**A fan is never powered from an FPGA I/O pin.** An I/O sources only a few mA; a fan pulls 100–500 mA. The FPGA supplies only the **control** signal (and optionally reads RPM).

### If it's a 4-wire (PC-style) fan — easiest, no extra parts
The PWM pin drives a MOSFET *inside* the fan; it's a 25 kHz logic input, 5 V-spec but **3.3 V-compatible**.

| Fan wire | Connect to | Notes |
|----------|-----------|-------|
| **+12V** (or +5V) | **12 V** external supply for a 12 V fan; header **5 V** pin only for a 5 V fan | The board has no 12 V rail |
| **GND** | header **GND** | must share ground with the fan's supply |
| **PWM** (control) | a **JM2 3.3 V PL I/O** | 25 kHz PWM from the FPGA; add a **10 kΩ pull-down** to GND |
| **TACH** (sense) | a **3.3 V PL input** | open-collector; add a **4.7 kΩ pull-up to 3.3 V** to read RPM |

### If it's a 2- or 3-wire fan — add one transistor
The FPGA can't switch motor current, so use an **N-channel logic-level MOSFET** (e.g. AO3400, 2N7002 for tiny fans) low-side:

```
  +12V/+5V ──────────────┐
                         (fan +)
                        [ FAN ]
                         (fan -)
                          │
              Drain ──────┘
   FPGA PWM ──[100Ω]── Gate      N-ch MOSFET
              Source ─────┐
                          │
   10kΩ Gate→GND         GND (shared with FPGA GND)
```
Fan speed = PWM duty on the gate. Keep the PWM in the low-kHz range for a bare MOSFET (a 4-wire fan wants 25 kHz; a MOSFET-switched 2-wire fan is happier at ~1–20 kHz).

---

## 4. Is pin assignment "an FPGA perk"? — yes

On a microcontroller, PWM only comes out of fixed timer pins. On the Zynq PL you write a
small PWM generator in HDL and **bind it to any free 3.3 V-bank I/O** via the `.xdc`:

```tcl
## Fan control — EXAMPLE. Replace <BALL> with the real PACKAGE_PIN from §5.
set_property PACKAGE_PIN <BALL>   [get_ports fan_pwm]
set_property IOSTANDARD  LVCMOS33 [get_ports fan_pwm]

set_property PACKAGE_PIN <BALL2>  [get_ports fan_tach]
set_property IOSTANDARD  LVCMOS33 [get_ports fan_tach]
set_property PULLUP      true     [get_ports fan_tach]
```

A 25 kHz PWM at, say, 100 MHz fabric clock is just a counter: period = 100e6 / 25e3 = **4000 ticks**; duty = compare value 0–4000. (Alternatively the PS can drive it via an AXI Timer / TTC through EMIO if you're running Linux on the ARM side — but a PL counter is simpler and jitter-free.)

**You cannot finalize the `PACKAGE_PIN` until §5 is filled from the board file.** Everything else (the PWM logic, the IOSTANDARD, the pull-up) is ready.

---

## 5. Pinout sheet — FILL FROM THE PUZHI BOARD FILE

> Leave a cell blank until you've confirmed it. Do **not** guess a ball — a wrong 5 V/GND
> assignment kills the chip. `PACKAGE_PIN` comes from Puzhi's example `.xdc` (see §2).

### JM1 (⚠️ confirm this is a 3.3 V bank; if it's set to 1.8 V, don't put the fan PWM here)
| Header pin | Function (power/GND/IO) | FPGA net name | PACKAGE_PIN | Bank V |
|:----------:|:------------------------|:--------------|:-----------:|:------:|
| 1 |  |  |  |  |
| 2 |  |  |  |  |
| … | *(fill 1–40 from the board file)* |  |  |  |

### JM2 (target header for the fan — set/confirm 3.3 V)
| Header pin | Function (power/GND/IO) | FPGA net name | PACKAGE_PIN | Bank V |
|:----------:|:------------------------|:--------------|:-----------:|:------:|
| 1 |  |  |  |  |
| 2 |  |  |  |  |
| … | *(fill 1–40 from the board file)* |  |  |  |

**Once filled**, pick one free 3.3 V IO for `fan_pwm` and one for `fan_tach`, drop their
`PACKAGE_PIN`s into the §4 `.xdc` snippet, and you're done.

---

## 6. FPGA + Orange Pi 4 Pro — what the Zynq actually adds

The Orange Pi 4 Pro (Allwinner **A733**: 2× A76 + 6× A55 + RISC-V, up to 16 GB **LPDDR5**,
**GPU + NPU with INT8/INT16/FP16/BF16**) already has strong graphics and AI. So the Zynq is
**not** useful as a "GPU." Its value is the things a Linux SBC does badly:

- **Real-time RF / DSP** — FFT / DDC / filtering on the 220 DSP48 slices at line rate (the SDR path).
- **Hard-real-time & custom I/O** — nanosecond-precise triggers, protocol bit-banging, logic analysis — no OS jitter.
- **Massively parallel bit-level compute** — the "matrix engine" role.

**Linking the two (highest → simplest bandwidth):**
| Link | On A733 | On Zynq | Notes |
|------|---------|---------|-------|
| M.2 PCIe 3.0 | ✅ M-key slot | needs a PL PCIe endpoint | highest BW, heavy to build |
| Gigabit Ethernet | ✅ | ✅ (2×) | **pragmatic high-BW path**, standard sockets |
| USB | ✅ USB 3.0 host | USB 2.0 host only | speed mismatch; awkward |
| SPI / UART / GPIO | ✅ 40-pin header | ✅ JM1/JM2 | simplest for control + modest data (like the Teensy link) |
| MIPI CSI | ✅ 2+4 lane in | — | feed processed data in as a "camera" |

Recommended: **Ethernet** for bulk data Zynq→OPi, **SPI/UART over the headers** for control.

---

## 7. Sources
- Puzhi PZ7020-StarLite product page — https://www.en.puzhi.com/detail/374.html
- Puzhi 2025 product guide (PDF, has the StarLite spec table) — https://macrogroup.ru/upload/iblock/5e3/o5hthn50zok4i0zuydafulso6pvlde0u/2025-PuZhi_web.pdf
- Orange Pi 4 Pro / Allwinner A733 — https://www.cnx-software.com/2025/10/25/35-orange-pi-4-pro-an-allwinner-a733-edge-ai-sbc-with-up-to-16gb-lpddr5-wifi-6/
- 4-pin fan PWM is 25 kHz, logic-level, 3.3 V-compatible — https://projecthub.arduino.cc/tylerpeppy/25-khz-4-pin-pwm-fan-control-with-arduino-uno-29961e
- 4-pin fan PWM drives an internal MOSFET — https://forum.allaboutcircuits.com/threads/understanding-pwm-on-a-4-pin-fan.185394/

*Full schematic / exact pin map: doc bundle from the seller (support@aithtech.com — Dropbox link + password on request), not public.*
