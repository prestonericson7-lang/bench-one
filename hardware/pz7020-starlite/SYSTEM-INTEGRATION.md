# PZ7020-StarLite in the system — what the real board data changes

Written after the vendor bundle arrived (manual, schematic, pin/length file, datasheets) and the
owner's own reference repo (`prestonericson7-lang/pz7020-starlite`: measured facts M-001…M-004,
printable pinout sheet, fan design). Everything earlier that was written without those documents
is corrected here; the corrections are listed in §6.

Related: [README.md](README.md) (pinout) · [PS-CONFIG.md](PS-CONFIG.md) (PS7 settings) ·
[FPGA-CAPABILITY.md](FPGA-CAPABILITY.md) (what the fabric is for) ·
[constraints/](constraints/) (master XDC, PS7 Tcl) · [BOOT-SD-runbook.md](BOOT-SD-runbook.md)

---

## 1. How the Zynq physically attaches to the rest of the car system

```
                     ┌──────────────────────── PZ7020-StarLite ────────────────────────┐
  Teensy 4.1 SPI0 ───┤ JM1 pins 9/11/13/15  (E18/E19/G17/G18)  BANK35 @3.3 V  ── PL   │
  (pins 13/11/12/14) │                                                                 │
  4-wire fan ────────┤ JM1 pins 1/3/5/7     (5 V, GND, H16 PWM, H17 tach)      ── PL   │
  mic array (future)─┤ JM2 / JM1 spare pairs, I²S or PDM                       ── PL   │
                     │                                                                 │
  Orange Pi 4 Pro ◄──┤ RJ45 "PS" ── RTL8211F #1 (addr 1) ── GEM0 on MIO16-27  ── PS   │
   (GbE switch)      │ RJ45 "PL" ── RTL8211F #2 (addr 2) ── BANK34 RGMII ── GEM1/EMIO  │
                     │                                                                 │
  RTL-SDR dongle ◄──►┤ USB 2.0 host (USB3320C on MIO28-39)                     ── PS   │
  console ───────────┤ Type-C "UART" (CH340E → MIO10/11)   Type-C "PWR+JTAG"   ── PS   │
  boot ──────────────┤ microSD (MIO40-45) | QSPI 128 Mb (MIO1-6) | jumper J1           │
                     └─────────────────────────────────────────────────────────────────┘
```

| Link | Physical | Level | Facts that fix the design |
|---|---|---|---|
| **Teensy ↔ PL, SPI** | JM1 pins 9/11/13/15 + GND pin 3 | 3.3 V both ends — **no shifter** | BANK35 default 3.3 V [M 3.7]; pins in one header column; `sdr_accel.xdc` carries the balls |
| **Pi ↔ PS, Ethernet** | PS RJ45 → 12 V GbE switch → Pi | — | GEM0 is hard-wired to the PS PHY via MIO; Linux `eth0` with no PL design at all |
| **Second Ethernet** | PL RJ45 | — | PHY #2 is plain RGMII on BANK34 → GEM1-over-EMIO + GMII-to-RGMII IP gives Linux `eth1`; a dedicated link (e.g. straight to the Pi, leaving the switch for everything else) |
| **USB device on the Zynq** | USB 2.0 host | — | ULPI on MIO — usable from Linux without PL work; the RTL-SDR can hang here if the SDR path moves to the Zynq |
| **Power** | Type-C PWR+JTAG **or** JM 5 V pin — never both | 5 V / 1 A [M 2.2] | `VDD_5V` is one hard-paralleled net (owner repo, schematic sheets 2/18/19). Feed the Zynq from the car's 12 V→5 V converter on its own branch; share **ground only** with the Teensy and the Pi |
| **Time master** | PL 50 MHz oscillator U18 + PS 33.333 MHz | — | Fabric counter clocked from U18 stamps SPI transactions; ±50 ppm crystal, no OS jitter |

## 2. What runs where (unchanged roles, now with the pin-level path)

| Job | Where | Path |
|---|---|---|
| FFT / DDC accelerator for the RTL-SDR node | PL, `sdr_accel_zynq_top` | Teensy SPI → JM1 → `spi_slave.v` → `fft_engine.v`; LEDs R19 heartbeat / V13 activity |
| Audio front-end (beamform + AEC) | PL (DSP48) | mics on spare JM pairs → PL → result to PS DDR over S_AXI_HP0 → Pi over Ethernet |
| Logging aggregation, timestamps | PL counter + PS | SPI frames stamped in fabric; PS batches to the Pi's NFS share over GEM0 |
| Linux (control, networking, USB) | PS, SD boot | FSBL+U-Boot+kernel from microSD; console on the CH340 port at 115200 |
| Fan | PL PWM on H16, tach H17 | `ps7-axi/pl_regs.v` (duty register + rpm readback from Linux) or the owner-repo `fan_top.v` — coexists with the SPI pins (different JM1 pins) |
| Time master | PL 64-bit counter at FCLK0 | `pl_regs.v` TIME_LO/HI, read by `pl_regs.py`; published as `PL_TIME` by the agent |

The Zynq's own DDR (512 MB or 1 GB — see PS-CONFIG §1) holds everything PL-side owns: audio
history, capture staging, accelerator buffers. None of that ever lands in the Pi's 4 GB.

## 3. Bring-up order that never risks the board

1. **QSPI LED self-test** — jumper J1 = QSPI, power via the PWR+JTAG Type-C, LEDs blink
   (factory-burned demo, `03.Boot Test/readme.txt`). Proves rails + fabric. Owner repo P-01.
2. **First PL bitstream over JTAG** — `fan_top.v` (owner repo) or `sdr_accel_zynq_top.v`
   (this repo, `build_bitstream.tcl` — its FILLME guard is now satisfied). Expect LED1 ~1 Hz.
   Needs Vivado (PS-CONFIG §6).
3. **Header identification (P-11)** — drive H16 high, probe pin 5 of each header: settles
   which physical connector is JM1 (no silkscreen; the pinout sheet's square-pad rule helps).
4. **Teensy link** — 5 wires per `sdr_accel.xdc`; LED2 lights on CS. The Teensy driver and
   the RTL were verified bit-exact in simulation; this is the first hardware run.
5. **PS boot from SD** — no Vivado needed: U-Boot SPL carries the validated `ps7/ps7_init_gpl.c`,
   then mainline Linux from the SD image built by `linux/` ([PS-LINUX.md](PS-LINUX.md)); console on
   the CH340 port at 115200; `eth0` via GEM0 → `BOOT-SD-runbook.md`.
6. **Second Ethernet** — GEM1/EMIO + GMII-to-RGMII, `phy-mode rgmii-id` first.
7. Only then the benchmarks the owner repo lists as P-05…P-10.

## 4. Numbers that are documented vs. still unmeasured

| Fact | Status |
|---|---|
| PL clock 50 MHz on U18; PS clock 33.333 MHz | ✅ DOC (manual, schematic) |
| JM1/JM2 balls, bank split, trace lengths ≈1770 / 1690 mil | ✅ DOC (xlsx) |
| Both PHYs RTL8211F-CG, addresses 1 (PS) and 2 (PL) | ✅ DOC (schematic sheets 15/16) |
| MIO map: QSPI 1–6, UART0 10/11, GEM0 16–27 + 52/53, USB0 28–39 + rst 46, SD0 40–45 | ✅ DOC |
| Bank 0 = 3.3 V, bank 1 = 1.8 V (straps) | ✅ DOC (sheet 5) |
| DRAM: **16-bit bus, 512 MB, DDR3L** | ✅ settled 2026-09-24: the hardware-validated PetaLinux build for this board (`Hiroto-Nakano/PZ7020StarLite`) uses `16 Bit`, HIGHADDR 0x1FFFFFFF, `DDR 3 (Low Voltage)` — and records that the 1.5 V setting fails `DDR_INIT_FAIL`. Matches schematic V1.0. The manual's "1 GB" is wrong for this board |
| DDR rail | ✅ 1.35 V (DDR3L) — see above |
| JTAG bridge | ✅ DOC: FT232H (U17) + 93LC56B EEPROM (U18), schematic sheet 19 → USB 0403:6014, `openFPGALoader -c digilent_hs2` |
| PL PHY RGMII delay mode | ⚠️ straps NC — determine at bring-up |
| BANK13 (MIPI) VCCO | ⚠️ not in the manual |
| Utilisation / Fmax on the real xc7z020 | ✅ **measured with the open toolchain** (nextpnr-xilinx + Project X-Ray, no Vivado): fan_top 174 LUT, Fmax 243 MHz; SDR accelerator 1,056 LUT / 445 FF / 4 DSP48 / 7 RAMB18, Fmax 82 MHz at a 50 MHz target — [OPEN-TOOLCHAIN.md](OPEN-TOOLCHAIN.md) |
| XDC transcription: `pz7020_starlite_board.xdc` (113 PACKAGE_PIN lines) and `sdr_accel.xdc` (7) checked ball-by-ball against the vendor xlsx/manual by `tools/check_xdc.py` | ✅ all OK (2026-09-24); owner-repo `fan_jm1.xdc` balls H16/H17/R19/G14/U18 all present in the vendor tables |
| U-Boot SPL for this board, no FSBL | ✅ built 2026-09-24: `boot.bin` 131,192 B with a valid Zynq BootROM header (XNLX magic, checksum OK), `ps7_init`/`ps7_post_config` from the validated `ps7_init_gpl.c` linked into the SPL (`nm`), `u-boot.img` 1,328,712 B, board DTB carries every node — [PS-LINUX.md](PS-LINUX.md) |
| Zynq ↔ Pi software link | ✅ `firmware/telemetry-hub/zynq_agent.py` (runs on the Zynq, XADC temp / PL state / memory as KEY=value on :8091) + `hub.py --zynq` TcpSource; both selftests pass, fault raised when the Zynq drops |
| Linux for the PS, built here | ✅ 2026-09-24: kernel 6.12 `zImage` 11,846,144 B + DTB + 10 MB modules; Debian bookworm armhf rootfs 425 MB (python 3.11, ssh, agent as a systemd unit); SD image assembled by `linux/mk_sd_image.sh` — [PS-LINUX.md](PS-LINUX.md) |
| PS ↔ PL register link | ✅ `ps7-axi/` bitstream with the PS7 placed; `pl_regs.v` verified by an AXI-Lite BFM testbench; `pl_regs.py` + agent selftests pass — [OPEN-TOOLCHAIN.md §4b](OPEN-TOOLCHAIN.md) |
| Which physical header is JM1 | ❌ owner repo P-11 |

## 5. What is in the vendor bundle, and what is missing

| Folder | Contents | Usable? |
|---|---|---|
| 01.Datasheet | DDR3 MT41K256M16TW-107, AT24C64D, RTL8211FD, W25Q128JV, CH340E, Xilinx UG470/472/473/475/476/586 | ✅ |
| 02.Software Tools and Drivers | CH341 UART driver (Win/Linux), FTDI CDM driver (for the USB-JTAG), NetAssist | ✅ drivers only; use AMD's own tools otherwise |
| 03.Boot Test | readme: LED demo pre-burned in QSPI | ✅ |
| 04.Hardware | schematic PDF (19 sheets), connector pin/length xlsx, PCB DXF | ✅ |
| 05.Course_Demos | `4_HLS.rar`, 763 MB — **truncated**: the zip is part `1-004` of a four-part Takeout export | ❌ get parts 2–4 for the vendor's Vivado/HLS projects (they carry the PS7 DDR training values and a known-good FSBL) |

Bundle location on this PC: `C:\Users\Danie\Downloads\pz7020-bundle\` (extracted), manual PDF
in `Downloads\`. Text extractions (`manual.txt`, `schematic.txt`) sit beside them.

## 6. Corrections to earlier documents in this repo

| Earlier claim | Now |
|---|---|
| "1 GB → 2 chips / 32-bit bus" (README §1) | Schematic shows **one chip, 16-bit, 512 MB**; 1 GB only if two chips are fitted — physical check required (PS-CONFIG §1) |
| "Blocked on the AITH Dropbox bundle: any bitstream touching I/O, the fan pin, a safe FSBL" (FPGA-CAPABILITY §5) | Bundle is here. Master XDC written, `sdr_accel.xdc` FILLMEs replaced with real balls, PS7 Tcl written. Remaining blocker is **Vivado itself** |
| "Whether the PL-side GbE is usable from Linux on the PS" (interconnect.md open question) | Yes: plain RGMII PHY, address 2, GEM1-over-EMIO + GMII-to-RGMII IP |
| RTL-SDR SPI link "on an expansion header, FILLME" | JM1 pins 9/11/13/15 = E18/E19/G17/G18, 3.3 V, no shifter |
| Reset "internal POR only" | Board key U19 available, but it also POR-resets the PS — keep the internal POR for PL-only bitstreams |
