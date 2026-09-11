# 10 — BUILD SHEET: the neural array (v2)

The wiring plan for everything ordered, so it can be assembled the day the parts land and
bench-tested in a fixed order. Every pin is either verified against a datasheet, the Teensyduino
1.62 core source, an official pinout diagram, or marked **NEEDS SCHEMATIC** — nothing is guessed.

**v2 (2026-09-08) supersedes v1 in one respect: the roles.** v1 made the FPGAs the compute engines
and the Teensys the front end. That was backwards for what this machine is for. The roles are:

| Node | Job, in one line |
|---|---|
| **Teensy 4.1 ×9** | **The sprinters.** Pure compute. Weights in DTCM, tiles streamed in, nothing else on the core |
| **Luckfox ×4, ESP32-S3 ×15** | **Take every other job off the Teensys** — orchestration, datasets, checkpoints, logging, sensors, radios, cameras, telemetry |
| **FPGA ×2 (1 GB DDR3 each)** | **The Teensys' big memory and their bandwidth.** Serves DDR3 to the Teensys over the fastest bus a Teensy has, prefetches ahead of them, and takes the layers that don't fit when the machine is under load |
| **PSRAM ×60** | **Per-Teensy reserve.** 6 chips = 48 MB on every Teensy, so a Teensy under load never has to stop and ask |

Everything below is built around that.

**Still needed from you** (§7): the PZ7020-StarLite doc pack (schematic + pin assignment — SCFPGA /
Puzhi send it by Google Drive after purchase, ask them), and which ESP32-S3-WROOM-1 variant is on
the dev boards.

---

## 1. Final inventory

| Node | Qty | Chip | Role |
|---|---|---|---|
| **Teensy 4.1** | 9 (7 here, 2 coming) | i.MX RT1062 @600 MHz (816 available, §5.1), 512 KB DTCM + 512 KB OCRAM, 8 MB flash | **Compute array.** T1 also keeps the tested fabric / display / radio / Luckfox links |
| **ESP-PSRAM64H** | **60** | 8 MB QSPI, SOP-8 **1.27 mm**, 133 MHz (84 MHz across a 1 KB page) | **54 on the Teensys (6 each), 6 spares for solder yield** |
| **PZ7020-StarLite** | 2 | Zynq XC7Z020-2CLG400, 85K cells, 220 DSP48, **1 GB DDR3**, dual A9 @766 MHz, GigE ×2 (PS + PL), USB 2.0 host, SD, 2× 40-pin PL headers (32 IO each) | **Memory server + prefetcher + overflow compute + UART concentrator + Linux backbone** |
| **Luckfox Pico Mini B** | 2 | RV1103, 64 MB, 0.5 TOPS NPU, microSD | Orchestrator, dataset/checkpoint host, NPU inference |
| **Luckfox Pico** (51 mm) | 2 | RV1103, same silicon, microSD | Same |
| **ESP32-S3 dev board** | 15 (6 here) | S3-WROOM-1, CH343 | Sensors, radios, mesh, telemetry, embarrassingly-parallel side jobs |
| ESP32-S3 CAM | 1 | N16R8 + GC2145 | Camera input |
| E32R40T | 1 | WROOM-32E, 4.0" ST7796 | Display node (wired, doc 04) |
| **ATtiny85-20PU** | 2 | 8-bit AVR, 5 V | **Power sequencer + independent watchdog — now required, §5.8** |
| SOP/SSOP-8 breakouts | 50 | | Not needed for the backpack (chips solder to the PCB). Spares |
| microSD 4 GB | 10 | | Luckfox roots, Teensy local shards |
| MP1584EN buck | 5 | 5 V / 1.8 A | Sub-2 A zones only |
| MOSFET switch module | 6 | D4184 dual N-ch, 15 A | **Low-side — see §4, they cannot switch a 5 V zone that shares data links** |
| INA219 | 5 | | Per-zone current |
| CH340 USB-TTL | 5 | | Consoles |
| Logic analyser | 1 | 8 ch, 24 MSa/s | UART and control lines. **Cannot see the QSPI bus** (52–105 MHz) — those are verified by memtest, not by eye |
| 74HC fabric | as before | | T1's IO fabric, unchanged (docs 03/04). Plus **one 74HC138 per Teensy backpack** (9 of your 24) |

Not part of this sheet: 4× MG996R servos.

---

## 2. The memory ladder — why the wiring looks the way it does

A Teensy 4.1 can reach memory at exactly these speeds. This table decides everything else.

| Tier | Where | Bandwidth per Teensy | Basis |
|---|---|---|---|
| **DTCM** | on-chip, 512 KB (minus code in ITCM) | ~4.8 GB/s at 600 MHz, **×1.36 at 816** — single-cycle, scales with the core clock | derived from the RM (pair of 64-bit TCM buses at core clock) |
| OCRAM | on-chip, 512 KB | slower than DTCM, cached | — |
| **Local PSRAM, bank 0** | FlexSPI2 CS0, memory-mapped at `0x70000000` | **~40–47 MB/s** at the core's default 105.6 MHz QSPI clock with the core's CS timing; **~32–40 MB/s** with the bank decoder's CS padding (§5.1); half those at 52.8 MHz | derived, cycle-counted: a 32-byte line = 2 cmd + 6 addr + 6 wait + 64 data (+ CS setup/hold) clocks; the 64-byte AHB prefetch burst amortises better. Teensyduino 1.62 `startup.c` sets **105.6 MHz** (not 88 — that was older cores). PJRC: usable range 49–132 MHz |
| **Local PSRAM, banks 1–5** | same bus, selected by 3 GPIOs through a 74HC138 | same as bank 0 **while selected**; a bank switch costs a cache invalidate (~tens of µs) | derived; see §5.1 |
| **FPGA DDR3 window** | FlexSPI2 **CS1**, memory-mapped from `0x70800000`, up to ~240 MB visible | **~30–35 MB/s** per Teensy with the long-dummy v1 protocol, **~40–47 MB/s** with the v2 window (same bus as the PSRAM — shared, not additional); the FPGA side has ~1 GB/s to spare, so five Teensys can pull at once | derived, cycle-counted; see §5.2 |
| PSRAM on plain SPI (SPI1) | peripheral, not memory-mapped | **~3 MB/s** — 30 MHz is NXP's absolute LPSPI maximum and tCEM = 8 µs caps each transfer at 26 bytes | datasheet (IMXRT1060CEC Table 57 note 1; ESP-PSRAM64H §10.5). **Not used** — 15× slower than the QSPI pads |
| UART to FPGA | Serial1 | 0.6 MB/s at 6 Mbaud | PJRC |
| microSD | Teensy socket | cold tier, unmeasured here | — |

Three consequences:

1. **The FlexSPI2 pads are the only fast door into a Teensy.** FlexIO parallel was checked and
   rejected: on the Teensy 4.1 the DMA-capable FlexIO2 only has **4 contiguous bits** on the
   headers (pins 10/12/11/13 = bits 0–3, pins 8/7/36/37 = bits 16–19 — from `core_pins.h`), and
   FlexIO3, which does have 16 contiguous bits (pins 19,18,14,15,40,41,17,16,22,23,20,21,38,39,26,27),
   **has no DMA request** (`FlexIO_t4.cpp`: `0xff` for all FlexIO3 DMA sources). A Teensy feeding a
   FlexIO3 bus by interrupt is not sprinting. So both the PSRAM banks and the FPGA hang off
   FlexSPI2, and it is one shared bus of ~50 MB/s per Teensy.
2. **Weights that are used every step live in DTCM; everything else is streamed in tiles.** That is
   the doc 08 rule and it survives: 400 KB of int8 weights per Teensy at ~1.2 GMAC/s (1.6 at 816)
   is the sprint; anything read from PSRAM or DDR3 runs at the ~50 MB/s of the bus, whichever
   device it comes from.
3. **The FPGA's job is to keep the Teensys' tiles ready** — that is what "the FPGA picks up the
   load" means in wire terms. The Teensy never parses a command mid-tile; it reads a mailbox word
   in its FPGA window at the end of each tile.

**Overclock, verified in the core source (`clockspeed.c`, `boards.txt`, `tempmon.c`):**

| Menu | Core voltage the core sets | Label | Measured die temp (others) |
|---|---|---|---|
| 600 MHz | 1.250 V | stock | 54–55 °C free air (cmrwash, 72 °F room) |
| 720 MHz | 1.350 V | "(overclock)" | — |
| **816 MHz** | **1.425 V** | "(overclock)" — no cooling note in the menu | **69–70 °C free air; >80 °C with airflow restricted** (same thread) |
| 912 / 960 / 1008 MHz | 1.525 / 1.550 / 1.575 V (cap) | "(overclock, cooling req'd)" | — |

NXP's recommended maximum core voltage is 1.30 V (comment in `clockspeed.c`, from the datasheet);
everything from 720 up exceeds it. `tempmon.c` arms a **panic at 90 °C that vectors to the fault
handler → CrashReport → reboot**, and Paul's statement is to stay below 95 °C "if you want the chip
to last more than 3 years." So: **816 MHz is on the table for compute Teensys with a heatsink and a
fan over the stack**, and telemetry of `tempmonGetTemp()` from every node is part of the protocol,
not optional. A 30-day run at 816 without cooling is a reboot at hour N with no checkpoint. What
816 buys: +36 % on DTCM-resident work. What it does not buy: any speed on PSRAM, the FPGA window,
UART, or SPI — those clocks come from other PLLs and do not move.

---

## 3. Topology — who wires to whom

```
                        ┌───────────────────────────────┐
                        │   Gigabit switch, 5-port      │
                        └──┬─────────┬─────────┬────────┘
                           │ GigE(PS)│ GigE(PS)│
                    ┌──────┴──┐  ┌───┴─────┐   └── your PC (dev + monitoring)
                    │ FPGA-A  │  │ FPGA-B  │
                    │ 1GB DDR3│  │ 1GB DDR3│
                    └─┬─┬─┬─┬─┘  └─┬─┬─┬─┬─┘
        QSPI + UART   │ │ │ │      │ │ │ │      QSPI + UART   ← 6 + 2 wires per Teensy
       ┌──────┬───────┘ │ │ └──┐   │ │ │ └────────┐
       T1     T2       T3 T4  T5   T6 T7 T8      T9           9× Teensy 4.1
       │      └─ each Teensy: PSRAM backpack (6× 8 MB, 74HC138 bank select)
       ├── SPI1 + Wire → 74HC/MCP fabric  (unchanged)
       ├── SPI0        → 2.4in TFT        (unchanged)
       ├── Serial1     → ESP32-S3 radio   (unchanged, TESTED)
       ├── Serial2     → Luckfox Mini #1  (unchanged)
       └── Serial7     → E32R40T display  (unchanged)

       Luckfox Mini #2, Pico #1, Pico #2 → UART to FPGA PL   (control plane)
       ESP32-S3 ×15 → ESP-NOW mesh → one radio-hub S3 per FPGA → UART to FPGA PL
       ESP32-S3 CAM → UART to a Teensy or to PL
       ATtiny85 #1 → zone enables + heartbeat (independent of everything above)
```

**Per-FPGA PL IO budget** (64 IOs on the two 40-pin headers):

| FPGA-A | pins | FPGA-B | pins |
|---|---|---|---|
| 5× Teensy QSPI slave ports (T1–T5) | 30 | 4× Teensy QSPI (T6–T9) | 24 |
| 5× Teensy UART | 10 | 4× Teensy UART | 8 |
| 2× Luckfox UART (Mini #2, Pico #1) | 4 | 1× Luckfox UART (Pico #2) | 2 |
| 1× radio-hub S3 UART | 2 | 1× radio-hub S3 UART | 2 |
| CAM UART (if on PL) | 2 | — | — |
| **Total** | **48 of 64** | | **36 of 64** |

**Control plane vs data plane.** The Luckfoxes run the orchestrator and hold datasets and
checkpoints on microSD, and they speak the framed protocol over 921600-baud UARTs — 92 KB/s. That
is fine for commands and telemetry and useless for bulk. So **bulk data (datasets, weight
checkpoints) lives on the FPGA side** — its SD card and DDR3 — and moves over GigE. The Luckfox
tells the FPGA *what* to stage; the FPGA moves the bytes.

**T1 keeps every existing link.** Nothing tested gets rewired. T1's Serial3 (14/15), formerly
reserved for T2, goes to FPGA-A; T2 gets its own PL UART. Doc 02's Link C is superseded.

---

## 4. Power — read this before plugging anything in

### The budget

| Load | Each | Qty | Avg | Peak |
|---|---|---|---|---|
| Teensy 4.1 @600 + 6 PSRAM | ~150 mA avg / 250 peak (PJRC: ~100 mA core at 600; PSRAM active ~30 mA each when addressed, one at a time) | 9 | 1.35 A | 2.25 A |
| Teensy 4.1 @816 | ~1.8× core power (816/600 × (1.425/1.25)²) — **derived, measure it** | 9 | +0.7 A | +1.2 A |
| PZ7020-StarLite | rated 5 V / 1 A; DSP-heavy load higher | 2 | 2.0 A | 3.0 A |
| Luckfox ×4 | ~300 mA | 4 | 1.2 A | 1.6 A |
| ESP32-S3 | 60–100 mA idle/RX, **350–500 mA WiFi TX** | 15 | 1.5 A | **7.5 A if all TX at once** |
| E32R40T + CAM + fabric + INA219 | | | 0.75 A | 1.1 A |
| **Total at 5 V** | | | **~7.5 A (8.2 at 816)** | **~15.5 A (16.7 at 816)** |

**Five MP1584EN at 1.8 A each is 9 A — below peak.** Don't build a 16 A system on daisy-chained
USB; the sag looks like firmware bugs.

### The plan

1. **One real 5 V trunk**: Mean Well LRS-100-5 class (5 V / 18–20 A) or a 12 V/10 A brick feeding
   the bucks.
2. **Zones**, so a fault isolates and INA219 sees it:

   | Zone | Loads | Peak | Feed |
   |---|---|---|---|
   | Z1 | FPGA-A + T1–T5 | 3.5–4 A | trunk, switched (see 3) |
   | Z2 | FPGA-B + T6–T9 | 3–3.5 A | trunk, switched |
   | Z3 | 4× Luckfox | 1.6 A | MP1584 #1 |
   | Z4 | ESP32-S3 ×8 | 4 A | trunk, switched |
   | Z5 | ESP32-S3 ×7 + CAM | 3.5 A | trunk, switched |
   | Z6 | Display + fabric | 0.7 A | MP1584 #2 |

3. **The D4184 modules are low-side switches.** On that module GND is common with DC− and the
   MOSFETs open the load's *negative* — that is what "trigger switch drive module" means. Used on a
   5 V zone whose boards also share UART/QSPI grounds with other zones, that (a) leaves the zone's
   ground floating when off, so data lines backfeed through every board's ESD diodes, and (b)
   when on, puts Rds(on) × 4 A of offset between that zone's ground and everyone else's — enough to
   eat the 90 mV UART margin in doc 05. **So the modules go on the 12 V side**: switch the 12 V
   *input* of a per-zone buck (each zone gets its own buck, grounds stay bonded at 5 V), or use
   them only for loads with no data connection (fans, the display backlight). If the trunk is a 5 V
   supply directly, zone switching needs a **high-side** switch (P-MOSFET module or a relay) — not
   these. Either way, **star ground at the trunk**; with 16 A flowing a daisy-chained ground
   develops hundreds of mV between nodes.
4. **INA219 on each of five zones.** Z1 and Z2 first — the FPGAs are the unknowns.
5. **Power sequencing is now required, not nice-to-have.** The QSPI links (§5.2) tie each Teensy's
   FlexSPI2 pads to an FPGA bank with 33 Ω in series — a powered Teensy driving an unpowered FPGA
   bank backfeeds ~100 mA into its ESD diodes. **Order: FPGA zones up and configured → Teensy
   zones → ESP32 zones.** That is the ATtiny's job (§5.8).
6. **Stagger ESP32 WiFi TX in firmware.** 15 radios keying at once is 7.5 A of transient.
7. **Verify every MP1584 trimpot with a meter before connecting a load.** They ship at random
   voltages.
8. **Bulk capacitance**: 470 µF at each ESP32 zone feed, 220 µF at each FPGA, 100 µF at each
   Teensy backpack (six PSRAMs plus a core at 816 is a bursty load).

### Thermal

~40 W average in the enclosure at 600 MHz, more at 816 and under training. Heatsinks on the two
Zynqs first, then on every Teensy that runs above 600 (14 × 14 mm with thermal tape on the
RT1062), and a fan over each Teensy stack. `tempmonGetTemp()` from every Teensy and the Zynq's
XADC temperature go into telemetry from day one.

---

## 5. Per-node wiring

### 5.1 Teensy 4.1 × 9 — the PSRAM backpack (6 chips, 48 MB, one bus)

**What the Teensy gives you.** Two underside SOIC-8 sites share one QSPI bus (FlexSPI2 port A:
SCLK, DATA0–3) and differ only in chip-select — `startup.c`: `GPIO_EMC_24 = FLEXSPI2_A_SS0_B
(RAM)` for the **small pads**, `GPIO_EMC_22 = FLEXSPI2_A_SS1_B (Flash)` for the **large pads**. The
core probes CS0 for a PSRAM (Read-ID `0x9F` must return `0x0D 0x5D`), maps it at `0x70000000`,
then probes CS1 and maps a second chip immediately after the first (`FLEXSPI2_FLSHA2CR0 = size2`,
address `size1 << 20`). `EXTMEM` and `extmem_malloc()` use whatever it finds. The AHB window is 32
MB in the linker (`ERAM ... LENGTH = 32768K`) and the MPU marks `0x70000000` 32 MB write-back.

**What we do with it.** Nothing is soldered *on* the pads. Nine short wires run from the pad sites
to a small PCB under the Teensy — **the backpack** — which carries the six PSRAMs, one 74HC138, a
bank-select header, and a 6-pin header out to the FPGA.

| Wire | From Teensy underside | To backpack |
|---|---|---|
| SCLK | EMC_25 pad (either site's CLK pin) | all six PSRAM CLK + FPGA header CLK |
| D0, D1, D2, D3 | EMC_26/27/28/29 | all six PSRAM SI/SO/SIO2/SIO3 + FPGA header D0–3 |
| **SS0** (small site, CE#) | EMC_24 | **74HC138 pin 4 (G2A, active-low enable)** |
| **SS1** (large site, CE#) | EMC_22 | **FPGA header CS** — direct, no decoder |
| 3V3, GND | Teensy 3V3 / GND | backpack planes; 100 nF at every PSRAM, 100 µF bulk |

**Bank decode**, static address inputs, enable on the CS path:

| 74HC138 pin | Signal | Note |
|---|---|---|
| 1, 2, 3 (A, B, C) | **Teensy pins 23, 24, 25** (BANK0/1/2) | **10 k pull-DOWN each** → bank 0 at power-up, before the sketch runs, so `startup.c` finds bank 0 and `EXTMEM` works |
| 4 (G2A) | SS0 from the Teensy | the only fast edge through the chip |
| 5 (G2B) | GND | |
| 6 (G1) | 3V3 | |
| 15, 14, 13, 12, 11, 10 (Y0–Y5) | CE# of PSRAM 0–5 | Y6, Y7 unused (populate chips 7–8 later if you buy more) |
| 16 / 8 | 3V3 / GND | 100 nF |

Pins 23/24/25 are free on all nine boards (T1's allocations are in `bench_pins.h`; T1 uses 2/3
for reserved RTS/CTS and 20/21/22 for the encoder, so 23–25 were chosen deliberately).

**Why the decoder delay does not break the bus.** The '138 adds its G2A→Y propagation delay to the
chip-select edge — 74HC at 3.3 V is slow (onsemi's 3.0 V column: 125 ns worst case at 85 °C; typ
~25 ns at room). FlexSPI has a register for exactly this: `FLSHCR1` **TCSS / TCSH** = chip-select
setup / hold in serial-clock cycles, **5-bit fields, 0–31 cycles** (`imxrt.h`). The core sets 1.
We set TCSS = TCSH = 14 on port A1: 14 × 9.5 ns = 133 ns at 105.6 MHz, which covers the worst-case
'138 even hot. Cost: 28 extra clocks on every transaction — a 32-byte line goes from 80 to 106
clocks (42 → 32 MB/s), a 64-byte prefetch burst from 144 to 170 (47 → 40 MB/s). That is the
"32–40" in §2. If you want it back, a 74LVC138A (~4 ns at 3.3 V) drops in and TCSS goes to 2; it
is optional and it is the only part not already on the bench.

**Why six chips on one bus is the risk, and what to do about it.** Six PSRAM inputs plus the FPGA
stub plus PCB trace on SCLK and D0–3 at 105.6 MHz is the signal-integrity question of this build.
The ESP-PSRAM64H limit is 133 MHz, 84 MHz for bursts that cross a 1 KB page — a 32-byte cache line
never does, so the core's 105.6 MHz is inside spec for the Teensy's own traffic. Plan:

- Keep the pigtail ≤ 3 cm, the backpack traces short, ground plane under everything, 33 Ω in
  series on SCLK at the Teensy end.
- **B2 memtests at 52.8 MHz first** (`CCM_CBCMR_FLEXSPI2_PODF(9)`, 528/10), then 105.6. If 105.6
  fails on any bank, the whole board runs at 52.8 — one SCLK feeds every device on the bus,
  PSRAM and FPGA alike. Budget with 52.8 (25 MB/s) until the first backpack passes at 105.6.
- The analyser cannot see this bus. Pass/fail is `extmem` memtest on every bank plus a
  CRC-of-a-known-pattern read-back a thousand times.

**Timing rules the firmware must keep** (datasheet §5.5, §10.5): **tCEM — CE# low for at most
8 µs**, or refresh is blocked and the chip corrupts; tCPH — CE# high ≥ 50 ns between bursts. A
32-byte line at 105.6 MHz holds CE# for ~1 µs, fine. Long IP-command bursts must be split.

**Bank switching, the rules:**

1. The CS0 window (`0x70000000`, 8 MB) gets its own MPU region set **write-through** (`MEM_CACHE_WT`
   in `startup.c` terms) instead of the core's write-back, so a bank switch needs only an
   invalidate, never a flush — a dirty write-back line landing in the wrong bank is silent
   corruption.
2. Switch = `arm_dcache_delete(0x70000000, 8 MB)` → write BANK0/1/2 → reset FlexSPI2's AHB read
   buffers (they prefetch 2 × 64 bytes and would hand back the old bank) → resume. Tens of µs.
3. Bank 0 is the only bank `EXTMEM`/`extmem_malloc` know about. Banks 1–5 are raw 8 MB windows
   owned by our tile loader. Nothing in Teensyduino ever switches a bank behind us.

**Solder order per board:** '138 + passives → PSRAM 0 only → wire to Teensy → B2 at 52.8 →
remaining five chips → B2 again at 52.8 → 105.6. Six chips at once on a board that fails tells
you nothing.

**Total:** 9 × 6 = 54 chips, **432 MB of PSRAM across the array, 48 MB per Teensy**, 6 spares.

### 5.2 Teensy 4.1 × 9 — the FPGA memory window (FlexSPI2 CS1) + UART

**The path.** The FPGA is the second device on each Teensy's QSPI bus: SS1 is its chip-select, and
in the PL it is a **QSPI slave that bridges to DDR3 over an AXI HP port**. To the Teensy it is
memory at `0x70800000`. Two ways to run it, and the sheet plans for both:

| | v1: direct bridge | v2: BRAM-windowed prefetch |
|---|---|---|
| PL design | QSPI slave → AXI master. Small, no cache, no coherence tricks | QSPI slave → per-Teensy 64 KB BRAM windows; a PL DMA engine fills windows from DDR3 ahead of the Teensy |
| Protocol on the wire | **Our own LUT for CS1** (`FLSHA2CR2` `ARDSEQID`/`AWRSEQID` point at our sequences): 32-bit address, **long dummy (e.g. 32 cycles ≈ 300 ns)** so the FPGA can fetch from DDR3 before it must drive data. Aperture up to ~240 MB | **Exactly the PSRAM protocol** (0xEB, 24-bit, 6 dummies) — the FPGA answers `0x0D 0x5D` to Read-ID and Teensyduino maps it as an 8 MB chip, no core changes. Window base set by a write to a control page |
| Per-Teensy rate | ~30–35 MB/s (a 32-byte line is 2 + 8 + 32 + 64 clocks ≈ 1.0 µs) | ~40–47 MB/s (PSRAM timing) |
| When | **First** — it is the smallest custom HDL block in the build (~300 lines) | when the FPGA starts "picking up the load" for real |

Both share the same wiring. The v2 window is where "good token rates under load" comes from: the
Teensy computes tile *n* while the FPGA has already put tile *n+1* in the window.

**Wires, per Teensy** (from the backpack's FPGA header to a PL header):

| Signal | Series R | Note |
|---|---|---|
| SCLK, D0–D3, SS1 | **33 Ω** at the driving end | 6 wires. Same ribbon, alternate with GND — at least 2 GND wires |
| GND ×2 | — | |
| **Ready/IRQ** (optional, v2) | 100 Ω | FPGA → Teensy **pin 16** (free on all nine) — "window filled" |
| Serial1 TX/RX (T2–T9), Serial3 14/15 (T1) | **100 Ω** | boot, debug, telemetry only. Commands ride the mailbox in the window |

**Ribbon length** ≤ 15 cm at 52.8 MHz; the 105.6 MHz case is the same measured question as the
backpack. **Which physical header pins on the StarLite — NEEDS SCHEMATIC** (§7 item 1). What is
known from Puzhi's page: two 40-pin 0.1" headers, 32 IO each, 3.3 V and 5 V pins present, **and the
headers are "optional solder" — check on arrival whether yours came populated.** The IO bank
voltage must be confirmed 3.3 V from the schematic before a single Teensy wire lands.

**Coherence rule for the shared DDR3:** each Teensy's region is its own during training (its
layer's weights and tiles); shared regions (a model everyone reads at inference) are read-only.
Cross-Teensy handoff is by explicit `arm_dcache_flush` on the writer, a mailbox word, and
`arm_dcache_delete` on the reader — the same discipline the fabric driver already uses for DMA.

**Baud for the UART:** start 921600 (proven). Raise to 6 Mbaud once the analyser confirms clean
edges (PJRC: 6 Mbit/s normal, 20 with a clock-source change). `addMemoryForRead()` is already in
the link class.

### 5.3 FPGA × 2 — PZ7020-StarLite

Known from the product page and your listing; the rest **NEEDS SCHEMATIC**:

| Item | Status |
|---|---|
| Power | 5 V / 1 A input, connector type NEEDS SCHEMATIC. Budget 1.5 A per board under DSP load |
| DDR3 | **1 GB**. Puzhi's family guide lists the StarLite DDR3 as **16-bit wide** → peak 2.1 GB/s at DDR3-1066, plan on ~1–1.4 GB/s sustained from PL. If the schematic shows two x16 chips (32-bit), double it |
| GigE PS | backbone to the switch |
| GigE PL | spare; FPGA-A ↔ FPGA-B direct later |
| USB 2.0 host | PS-side; a USB stick for datasets/checkpoints is the simplest bulk storage |
| SD | boot media (Puzhi's Linux first) |
| 2× 40-pin headers | 32 IO each; 3.3 V + 5 V pins; **"optional solder" — verify populated**; bank VCCO NEEDS SCHEMATIC |
| UART | PS console — first boot |
| JTAG | Xilinx-compatible cable (Platform Cable USB clone, ~$10) |
| MIPI CSI | present on the 7020; not used |

**PL blocks, in build order:**

1. **UART concentrator** — AXI UARTLite × N from the IP catalog, no custom HDL. Linux talks to
   them via UIO; `iop.py` runs unchanged on the A9.
2. **QSPI-slave-to-AXI bridge** (v1 of §5.2) — the one custom block that matters. One port first,
   memtest from one Teensy, then five ports behind an AXI interconnect on HP0/HP1.
3. **Windowed prefetcher** (v2) — BRAM windows + AXI DMA + a descriptor queue the orchestrator
   fills.
4. **Matrix engine** — DSP48 GEMM against DDR3 for layers larger than the array holds. This is
   the "overflow compute" and it is last, because 1–3 are what make the Teensys sprint.

**First-boot plan.** PYNQ ships prebuilt images only for official boards; a StarLite needs a custom
PYNQ image (Vivado + Vitis + PetaLinux + a Puzhi BSP, days). So:

1. **Boot the Linux image Puzhi ships** from SD. PS console, `free -m` (~1 GB), `ping` over GigE.
2. **Install Vivado** (free WebPACK covers the 7020; ~100 GB, budget a day).
3. Block 1 above. Then block 2.
4. PYNQ only if the Jupyter workflow is wanted later.

**Vivado is unavoidable for anything in PL.** There is no route around it on a Zynq.

**Documents.** Puzhi: "The Documents come with this product, saved in Google Drive/Yandex/Dropbox.
Contact Customer Service to get it after purchase." Your listing says HDL demos are included.
**Ask SCFPGA (the Amazon seller) for the PZ7020-StarLite doc pack now** — schematic, pin assignment
(`.xdc`), user manual, Linux image, demos. §5.2 and the header tables cannot be pinned without it.

### 5.4 Luckfox × 4

| Board | Link | Pins | Notes |
|---|---|---|---|
| Mini B #1 | UART3 → **T1 Serial2** | header 12 (TX) → T1 pin 7, header 13 (RX) ← T1 pin 8 | Unchanged from doc 05. 100 Ω series both wires |
| Mini B #2 | UART3 → FPGA-A PL | header 12/13 → PL UART pins NEEDS SCHEMATIC | Enable per doc 05 |
| Pico #1 (51 mm) | UART3 → FPGA-A PL | **header 19 (TX, GPIO1_D0, sysfs 56) → PL RX; header 20 (RX, GPIO1_D1, sysfs 57) ← PL TX** | Same SoC mux as the Mini (`UART3_M1`), same enable — **different physical pins: bottom of the LEFT column, not the right.** Counting from the Mini diagram lands on GPIO4_B0/B1 |
| Pico #2 (51 mm) | UART3 → FPGA-B PL | header 19/20, as above | 100 Ω series both wires. GND at 18 or 23 |

**Console lifeline, all four:** a CH340 on UART2 — **pins 4/5 on a Mini, pins 1/2 on a Pico** —
115200. Wire this *before* anything else.

**Measure the UART3 TX pad idle voltage on every board before connecting** (doc 05 §2) — pin 12
on a Mini, pin 19 on a Pico. VCCIO6 is 1.8/3.3 V selectable in silicon.

#### Luckfox pin reference — both boards, from the official pinout diagrams

Both are DIP-style: down one column, up the other. Both have a microSD slot (the Mini **B** does;
the Mini A does not). Both expose the 10/100 Ethernet pairs as bare pads — magnetics needed,
unmeasured, not used here.

| Signal | **Pico Mini B** (22 pins) | **Pico** 51 mm (40 pins) |
|---|---|---|
| UART3_TX_M1 (GPIO1_D0, sysfs 56) → link | **12** | **19** |
| UART3_RX_M1 (GPIO1_D1, sysfs 57) ← link | **13** | **20** |
| UART2_TX_M1 (console, GPIO1_B2) | 4 | 1 |
| UART2_RX_M1 (console, GPIO1_B3) | 5 | 2 |
| SPI0_CS0_M0 (GPIO1_C0) | 6 | 12 |
| SPI0_CLK_M0 (GPIO1_C1) | 7 | 14 |
| SPI0_MOSI_M0 (GPIO1_C2) | 8 | 15 |
| SPI0_MISO_M0 (GPIO1_C3) | 9 | 16 |
| SPI0_CS1_M0 (GPIO1_D2) | 15 | 9 |
| I2C3_SDA_M1 / SCL_M1 | 14 / 15 | 9 / 10 |
| UART4 TX / RX / RTS / CTS | 11 / 10 / 16 / 17 | 6 / 7 / 5 / 4 |
| SARADC_IN0 (GPIO4_C0, **boot strap — leave alone**) | 19 | 31 |
| SARADC_IN1 (GPIO4_C1, the usable one, 1.8 V max) | 20 | 32 |
| GPIO0_A4 | 18 | 17 |
| Extra GPIO4_A2/A3/A4/A6, B0/B1 | — | 25 / 26 / 27 / 24, 22 / 21 |
| 3V3 (OUT) | 3 | 36 |
| 1V8 (OUT) | 22 | **not exposed** |
| VBUS | 1 | 40 |
| VSYS / 3V3_EN | — | 39 / 37 |
| GND | 2, 21 | 3, 8, 13, 18, 23, 28, 33, 38 |
| NC | — | 29, 30, 34, 35 |
| ACT LED / USER button | GPIO3_C6 / GPIO1_A2 | GPIO3_C6 (sysfs 118) / — |

The sysfs numbers on the Pico diagram confirm the doc 05 formula: GPIO1_D0 = 1×32 + 3×8 + 0 = 56.

**No PSRAM on the Luckfoxes.** All 60 go to the Teensys (§5.1). A PSRAM on the RV1103's SPI0 would
be a ~6 MB/s peripheral against 64 MB of onboard DDR2 — it does nothing for the Teensys, which is
what the reserve is for.

### 5.5 ESP32-S3 × 15 + CAM

From the dev-board schematic: ESP32-S3-WROOM-1, CH343P on UART0 (GPIO43/44) via USB-C #1, native
USB on GPIO19/20 via USB-C #2, WS2812 on GPIO48, BOOT on GPIO0, two 22-pin headers.

| Role | Count | Link |
|---|---|---|
| Radio hub | 1 per FPGA = 2 | UART to FPGA PL, ESP-NOW to its nodes |
| Mesh nodes / sensor workers / side jobs | 12 | ESP-NOW only. Power + GND |
| The existing tested radio | 1 | T1 Serial1 — **unchanged** |
| CAM | 1 | UART to a Teensy, or to PL |

**UART from a hub S3:** GPIO17/18 exactly as the tested radio uses — proven clear of every S3 trap.
Max 5 Mbaud on S3. **Which WROOM-1 variant** (N8 / N8R2 / N16R8) — Open Item 2. **The hub must
stagger TX** across its nodes (power §4).

### 5.6 The 6 spare PSRAM + breakout boards

Spares for the 54 that get soldered — SOP-8 by hand has a yield. If a breakout is ever used, it is
the **1.27 mm side**; the 0.65 mm side is SSOP and the chip will not seat.

### 5.7 The fabric

Unchanged. Docs 03 and 04 stand. T1 owns it on SPI1 (26/27/39) and Wire (18/19). Its job in the
neural array is the **simultaneous-capture input layer** — 152 bits on one `/PL` edge plus 64
analog channels. T1 also gets a backpack and a FPGA window like every other Teensy; its
allocations already leave 23/24/25 and 16 free.

### 5.8 ATtiny85 × 2 — power sequencer and independent watchdog

Yes, they help, and after §5.2 they are required. Sequencing order is FPGA → Teensy → ESP32, and
the one processor that must keep working when everything else browns out is the one that is not
on the trunk's data buses. 5 V native (runs straight off the trunk), internal 128 kHz watchdog
oscillator independent of the CPU clock, brown-out detector fuse at 4.3 V so it holds zones *off*
through a sag rather than glitching them.

| ATtiny85 pin | Signal | To |
|---|---|---|
| PB0 (pin 5) | **heartbeat in** | FPGA-A PS GPIO or T1 pin 17, toggled ≥ 1 Hz by the orchestrator |
| PB1 (pin 6) | ZONE_FPGA enable | switch for Z1+Z2 12 V feed (or high-side switch) |
| PB2 (pin 7) | ZONE_TEENSY enable | (if the Teensys get their own bucks) |
| PB3 (pin 2) | ZONE_ESP_A enable | Z4 |
| PB4 (pin 3) | ZONE_ESP_B enable | Z5 |
| PB5 (pin 1) | RESET | **leave as RESET** — fusing it as IO needs a high-voltage programmer to ever reflash |
| VCC / GND (8 / 4) | 5 V trunk, star ground | 100 nF |

Behaviour: on power-up, enable zones in order with 500 ms between each. Then require a heartbeat
edge every 5 s; miss → drop the Teensy/ESP zones for 2 s and re-enable (a hardware power-cycle
recovery), count strikes, and after 3 in 10 minutes leave them off. Nothing about it speaks the
framed protocol — that independence is the feature. #2 is a spare, or a second watchdog on the
second stack. Program with a USBasp (~$3) or an Arduino as ISP.

---

## 6. Bench-test order

Each stage assumes the one before it passed. Out of order gives the wrong diagnosis.

| Stage | What | Pass looks like | Instrument |
|---|---|---|---|
| **B0** | Power tree alone. Set every MP1584. Confirm the D4184 modules are on the 12 V side or replaced with high-side switches. Measure every zone | Every rail 5.00 ± 0.15 V unloaded; zone grounds within 20 mV of each other under 2 A | Meter |
| **B1** | Existing stack (T1, T2, ESP radio, Luckfox Mini #1, display) — doc 03's L0–L7 | `benchctl sys` shows links up; self-test passes | Analyser, INA219 |
| **B2** | One backpack, one chip, 52.8 MHz | `extmem` memtest passes bank 0 | Serial monitor |
| B2b | Same board, all six chips, 52.8 → then 105.6 | Memtest passes every bank at the highest clock that passes; log the clock | |
| B2c | All nine boards | Same. The fails are the hand-solder yield | |
| **B3** | FPGA-A boots Puzhi's Linux from SD | Linux prompt on PS UART; `free -m` ~1 GB; `ping` over GigE | CH340 + switch |
| B3b | FPGA-B same | | |
| **B4** | One PL UART ↔ T2 at 921600, framed protocol | PING/PONG both directions, zero CRC errors over 10,000 frames | Analyser |
| B4b | 6 Mbaud | Same | Analyser confirms edges |
| B4c | All 9 Teensy links + 3 Luckfox + consoles | `sys` from FPGA-A shows its 5 + 2, FPGA-B its 4 + 1 | |
| **B5** | ATtiny sequencer in the loop; pull the heartbeat | Zones drop and return in order; INA219 shows the staggered inrush | INA219 |
| **B6** | ESP32 hub + 2 nodes, staggered TX | No TX-coincident sag | INA219 on Z4 |
| **B7** | **QSPI bridge v1: one Teensy ↔ FPGA-A DDR3** | Memtest of the CS1 window from the Teensy at 52.8 MHz; log MB/s | memtest + CRC |
| B7b | Five Teensys on FPGA-A at once | All five memtests pass concurrently; aggregate MB/s logged | |
| **B8** | Milestone 1 from doc 08: one RBM on one Teensy, CD-1, 14×14 data, weights in DTCM | Reconstruction error falls epoch over epoch; log s/epoch at 600 and 816 | |
| B9 | Same RBM with weights tiled from PSRAM banks, then from the FPGA window | Same curve; log the slowdown vs DTCM — that is the memory ladder measured | |
| **B10** | Pipeline: T1 fabric captures → RBM layer → next Teensy → FPGA stages tiles | End to end, one clock discipline | Marker pin + analyser |
| B11 | Windowed prefetch v2 | Teensy never waits on a tile; window-ready latency logged | |

B0 through B4c is a weekend once parts are here. B7 is the first real FPGA work and it is the
build's centre of gravity.

---

## 7. Open items — the sheet is incomplete without these

1. **PZ7020-StarLite doc pack** — schematic, pin assignment, user manual. Needed for: header pin →
   PL bank/IO, bank VCCO, DDR3 width, power connector, PS UART pins. Ask the seller (SCFPGA) —
   Puzhi delivers it by Google Drive after purchase.
2. **ESP32-S3-WROOM-1 variant** printed on the module can (N8, N8R2, N16R8…).
3. ~~Luckfox Pico (51 mm) pinout~~ — **CLOSED.** Official pinout received; table in §5.4.
4. **A 5 V trunk supply** (≥ 18 A) or a 12 V/10 A brick + per-zone bucks — not in the current orders.
5. **A 5-port gigabit switch** and 3× CAT5 — not in the current orders.
6. **JTAG cable for the Zynq** — optional if SD boot works, essential if it doesn't.
7. **Are the 40-pin headers soldered on your StarLites?** The listing says optional. If not: 4×
   2×20 0.1" headers and a steady hand.
8. **The 9 backpack PCBs** — a small two-layer board (~40 × 25 mm): 6× SOP-8 (1.27 mm), 1× DIP-16
   or SOIC-16 74HC138, 3 pull-downs, decoupling, 100 µF, a 9-pin pigtail pad row, a 3-pin bank
   header, an 8-pin FPGA header. You said you can order PCBs; this is the first one.
9. **Heatsinks + fan** for the Teensy stacks if 816 is used; the FPGA heatsink kit goes on first
   regardless.

---

## 8. Software roles, so firmware and wiring agree

| Node | Runs | Speaks |
|---|---|---|
| **T2–T9** | The compute loop only: DMA tile in → SMLAD/ternary kernel in DTCM → result out. Bank switcher. Telemetry (temp, tile rate) once per tile, not per byte | Mailbox in the FPGA window; UART for boot/debug |
| **T1** | Fabric master, display, existing links, **capture front end** — plus the same compute loop when idle | Unchanged + Serial3 to FPGA-A + its own window |
| **FPGA PS (A9, Linux)** | Bulk store (SD/USB/DDR3), tile staging queue, checkpoint writer, `iop.py` speaker, GigE to your PC | Ethernet; framed protocol over PL UARTs |
| **FPGA PL** | UART concentrator → QSPI bridge → windowed prefetcher → matrix engine, in that order | AXI to PS |
| **Luckfox ×4** | `orchestrator.py` as-is — the control plane: schedules layers, epochs, checkpoints; NPU inference of trained layers (RKNN) | UART |
| **ESP32 hubs / nodes** | Radio hub (to write) / sensors, side jobs (to write) | UART + ESP-NOW |
| **ATtiny85** | Sequencer + watchdog (to write, ~100 lines) | One heartbeat wire in, four enables out |

**Firmware to write, in order** (all on the tested protocol, all additive):

1. `bench_extmem_banks.{h,cpp}` — bank select on 23/24/25, WT MPU region for CS0, invalidate +
   FlexSPI2 buffer reset on switch, TCSS/TCSH = 14, FlexSPI2 clock select 52.8/105.6, per-bank
   memtest. **Needed for B2.**
2. `bench_fpga_window.{h,cpp}` — CS1 LUT (32-bit address, long dummy), `FLSHA2CR0` aperture, MPU
   region, mailbox struct, flush/invalidate helpers. **Needed for B7.**
3. Compute-loop skeleton for T2–T9 (tile DMA from a window into DTCM, double-buffered).
4. ATtiny85 sequencer.
5. PL: UARTLite design, then the QSPI bridge.

The framed protocol and its five new channels (`bench_protocol.h`) carry all of this unchanged.

---

## 9. What this build is

Nine Cortex-M7s at 600–816 MHz, each with 48 MB of its own PSRAM and a memory-mapped window into
a gigabyte of DDR3 behind a Zynq that stages tiles ahead of it, so the cores spend their cycles on
weights and not on waiting. Four Linux nodes run the show, fifteen radios do the sensing, one
discrete-logic front end captures 152 channels at an instant, and one 8-pin AVR decides when
everyone gets power. One protocol. One clock discipline. Under $1,500 of parts — and the biggest
PSRAM array anyone has hung off a Teensy.
