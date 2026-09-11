# Fabric research (datasheet-level, with sources)

Raw findings from the 7-agent research pass. Numbers verified against primary datasheets.
Keep this as the reference so the design doc's claims can be traced.

## A. MCP23017 — I2C GPIO layer
- Address `0100 A2A1A0` → 0x20–0x27. A0/A1/A2 are TTL buffers, no internal bias — hard-strap
  (10k to GND + jumper to VDD). 8 chips = 128 IO/bus.
- Bus: 100k @1.8V+, 400k @2.7V+, **1.7M needs VDD≥4.5V → at 3.3V you're capped at 400k.** Cb max
  400 pF. Pull-ups @3V3: use **2.2k** (1.5k if Cb>150 pF); Rp_min ≈1k. 8 chips + ≤30 cm ≈ 100–150 pF.
- Current: 25 mA/pin abs, **150 mA out of VSS / 125 mA into VDD** package caps. Guaranteed levels only
  at IOL 8 mA / IOH −3 mA → **sink-drive LEDs, never source-drive.** Internal pull-ups are 40–115 µA
  (~100k) — too weak for off-board buttons, use external 10k.
- Registers (BANK=0 default): IODIR 00/01 … GPIO 12/13, OLAT 14/15, IOCON 0A&0B. SEQOP=0 → one
  transaction from 0x12 reads GPIOA+GPIOB (16 in) in ~123 µs @400k; 8 chips ≈ 1 ms full scan.
- INT: MIRROR=1 + ODR=1 (open-drain), tie all INTs to one Teensy IRQ, 4.7k to 3V3, attach FALLING.
- **Gotchas:** 8×4.7k = 590 Ω (dead bus) → one pull-up pair only. **MIRROR=1 doesn't clear on a
  single-port read → always read BOTH ports** or ISR wedges. Read INTCAP before GPIO. Float on
  RESET/A-pins → random address/lockup. IOCON mirrored at 0x0A and 0x0B. 0x20–0x27 collides with
  PCF8574 + many I2C radios — reserve the block. ESP+Luckfox on same segment = multi-master (avoid).
- Sources: Microchip DS20001952C; errata DS80252A; Adafruit mirror DS21952B.

## B. 74HC138 — SPI chip-select decode
- DIP-16. Pins: 1 A0, 2 A1, 3 A2, 4 /E1, 5 /E2, 6 E3, 7 Y7, 8 GND, 9 Y6…15 Y0, 16 VCC.
- Outputs all HIGH unless E1=L·E2=L·E3=H → one Yn LOW. **Dedicate E3 as global /CS gate.** Use HC not HCT.
- Timing: no 3.3V column in datasheets; ~20–25 ns typ, **budget 100 ns settle** (delayNanoseconds(100)).
  Sinks CS logic only (~1 µA), no power/LEDs.
- Wiring: A0-2←Teensy 2/3/4; EN←Teensy 33 + 10k pull-down; Yn→100Ω→/CS + 10k pull-up per port.
  **Break-before-make.** Cascade: 2 chips→1-of-16 (enable=A3); master+leaves→32/64 (5 chips=32, 9=64).
- **Gotchas:** floating EN at boot asserts a random CS → corrupts SD/flash (#1 failure — hence the
  pull-down). MISO contention not fixed by CS (bring up one port at a time). Never float A/E inputs.
- vs 595: 138 fast/atomic/3–4 pins; 595 unlimited but ~1–3 µs/change and shift traffic hits whatever
  is selected. Hybrid: 595 for slow lines, 138 owns CS.
- Sources: Nexperia 74HC_HCT138; TI SN74HC138; Nexperia 74HC_HCT595.

## C. 74HC595 / 74HC165 — shift IO
- 595 DIP16: 14 SER, 11 SRCLK, 12 RCLK, 13 /OE, 10 /SRCLR, 9 QH', 15+1..7 QA..QH. **RCLK latch =
  atomic output.** Never tie SRCLK=RCLK. ~20 MHz @3.3V. **±70 mA package total = the real LED cap**
  (per-pin 3–5 mA @3.3V). /OE→PWM = global dim (active low). >5 mA/pin → TPL7407LA (3.3V-correct) / ULN2803.
- 165 DIP16: 1 /PL, 2 CLK, 15 CLK_INH→GND, 9 QH, 10 SER(cascade in), 11..14+3..6 = A..H. /PL LOW =
  async snapshot. **H exits first → bit7=H..bit0=A.** t_pd 30 ns max.
- Both mode 0 → share SCK: 595 on MOSI, 165 on MISO. **First byte → farthest 595 (reverse buffer).**
  Independent 595 latching from GPIO uses 74HC238 (active-high), not 138.
- Chain limit = shared SCK/RCLK/PL net; past ~8 chips buffer with 74HC541 identically per chip.
- **Gotchas:** 165 QH always drives → don't share MISO (own SPI port or 74HC125 gate). /SRCLR float =
  random clears. /OE high = Hi-Z (pull-downs on MOSFET-gate drivers). Use HC not HCT. 165 inputs are
  where a 5 V sensor sneaks in → 1k series/divider (VCC+0.5 abs max).
- LA verify: 8×N edges between /PL↑ and RCLK↑; RCLK strictly after last SCK; walking-1 to map bits.
- Sources: TI SN74HC595, SN74HC165; Nexperia 74HC165/595; TI E2E 595-at-3.3V.

## D. 74HC4051 — analog mux tree
- DIP16: 16 VCC, 8 GND, **7 VEE→GND**, 3 Z, 6 /E, 11/10/9 = S0/S1/S2, channels Y0..Y7 = 13,14,15,12,1,5,2,4.
- VEE=GND → passes 0–3.3 V unipolar. **Ron ~150–300 Ω @3.3V, signal-dependent**; leakage ±1 µA →
  keep source ≤1–10k or buffer (MCP6002/TLV2372). Switch time ~30–58 ns; **wait 5–20 µs + discard first read.**
- Tree: 8 leaves, Z bussed to 1 ADC, S0-2 shared, a 74HC138 drives each /E → **64 ch, 6 GPIO, 1 ADC,
  one Ron in path** (beats master-mux's two Ron).
- ADC: Teensy 4.1 best (0–3.3V, 12-bit). ESP32-S3 nonlinear (>2900 mV undefined, ADC2 dead w/ WiFi).
  **Luckfox SARADC = 1.8 V, 10-bit, 2 ch; SARADC_IN0 is a boot strap → use IN1; scale 10k/12k→1.80 V
  after a buffer.**
- **Gotchas:** VEE float; signals outside GND–VCC latch-up; it's a switch not a buffer (passes noise).
- Sources: Nexperia 74HC_HCT4051; TI CD74HC4051; Rockchip RV1103 datasheet + Luckfox Pico Mini schematic; Espressif ADC docs.

## E. 74HC373 / 74HC04 / assortment glue
- **373** transparent latch (LE high = transparent, captures on LE fall; /OE tri-states). Use: parallel
  bus demux (bus → N ports, each held by its LE) and input snapshot. 374 = edge-triggered (Q never
  tracks). 573/574 = straight-through pinout (buy 573 before PCB). ~15 covers ~15 ports.
- **04** inverter: load-bearing use = 138 (active-low) → 373 LE (active-high) flip; clock fanout (2
  gates). NOT a power buffer; don't build RC osc from it (use 74HC14 Schmitt). ~10–15 ns/gate @3.3V.
- Assortment HC to use: 74HC138 (select), 74HC14 (debounce/edges), 74HC595/165 (SPI ports),
  74HC00/08/32 (strobe gating; make XOR from 4 NANDs). 74HC164 = SIPO w/o latch (never on CS/LE).
- **Avoid every 74LS at 3.3 V** (LS00/02/04/08/32/47/74/86/90/164/138/**245**). LS needs 4.75–5.25 V;
  LS output-high can exceed Teensy's 3.6 V abs max. **His only '245 is LS245 — do NOT use as the bus
  transceiver; buy 74HC245 / 74LVC245A.**
- Wiring: 100 nF/IC + 10 µF bulk; tie all unused inputs; 33–100 Ω series off-board; 10k pull-ups on
  /OE //CS, pull-downs on active-high LE (idle inactive at reset). One common star ground.
- **Gotchas:** two /OE low = shoot-through (enforce exclusivity in hardware). Unpowered module's input
  diodes clamp a live bus (isolate with a 245 or power modules together). Teensy's 8 "parallel" pins
  are scattered across GPIO ports → 8 writes unless you pick a contiguous port group or use FlexIO.

## F. Shifting / power / instrumentation
- **TXB0108** push-pull, ≤100 Mbps, internal 4k buffer + edge one-shots — **never on I2C** (any
  pull <50k breaks it). **TXS0108E** open-drain-capable (4k up / 40k down), ~1.2 Mbps open-drain — for
  I2C/pulled buses. Rule: pulled bus→TXS, push-pull→TXB. VCCA(3.3) ≤ VCCB(5) always; OE to VCCA, pull
  it (Hi-Z at power-up); stubs <10 cm.
- **Luckfox RV1103 IO domains:** VCCIO1/PMU 3.3V; VCCIO3/4/6 = 1.8 **or** 3.3 configurable; MIPI/GPIO7
  1.8V only. Header also has a 1.8V power pin. **Measure each pad idle-high before wiring.**
- Power budget: MCP 1 mA; 74HC 1–3 mA switching; 16 LEDs ≈80 mA; 2× Teensy ~200 mA; ESP 350–500 mA TX;
  Luckfox 200–300 mA. Typ ~1.2 A, peak ~1.5–1.8 A → **5 V rail ≥2.5 A.** MP1584 buck ≥1.5 A (verify
  trimpot first); AMS1117 practical ceiling ~400 mA. Decouple 100 nF/IC + 10 µF/section + 220–470 µF at ESP.
- **INA219:** high-side; addr 0x40 +4·A1+A0 (breakouts usually expose 0x40/41/44/45). Put total-stack
  sensor on 5 V side before the buck (0.1 Ω drops 150 mV @1.5 A); PGA/1 for the 100–300 mA fabric.
- **24 MHz LA:** ~8 MSa/s on 8 ch → cap SPI capture at ~4 MHz SCK; I2C/UART fine. GND every 2–3 taps.
  Toggle a spare GPIO as a marker to catch intermittents.
- 1N4148: signal clamp/steering + small-relay flyback (≤150 mA); **don't diode-OR the 5 V rail** (Schottky/ideal-diode instead).
- Sources: TI TXB0108, TXS0108E, SCEA054A, INA219; Rockchip RV1103 V1.4; Microchip MCP23017.

## G. Prior art + completeness audit
- Module-port standards to model: **mikroBUS** (2×8 0.1", diagonal notch key; L: 3V3/GND/PWM/INT/RX/TX/SCL/SDA,
  R: 5V/GND/AN/RST/CS/SCK/MISO/MOSI), **Pmod** (Type2 SPI / Type6 I2C / Type3 UART; 200Ω series + ESD),
  **Qwiic/STEMMA-QT** (JST-SH 1 mm, GND/VCC/SDA/SCL, 3.3 V, polarized). Avoid Grove/Gravity as a model
  (ambiguous/reversed pinouts). Good-port rules: keying, power pins outermost, one bus/port, series R +
  ESD at host, silk names both sides, port-type letter.
- Multi-MCU coordination prior art = the AMP/IPC world (OpenAMP RPMsg, Arm HIPC): steal "one owner per
  resource, endpoints/channels, mailbox + shared buffer, explicit boot/reset lifecycle." **Nobody has
  published this for a Teensy/ESP32/Linux-SBC bench stack — that's the genuine gap to claim.**
- Top pitfalls guides omit (ranked): floating 74HC inputs; I2C >400 pF / missing pull-ups; address
  collisions (publish a map, plan a TCA9548A); level-shifter direction failures on I2C; ADC source-Z /
  mux Ron; decoupling + ground bounce; hot-plug back-powering; SPI mode/CS timing per device; shared-bus
  contention / bus owner undefined; 74LS at 3.3 V; connector keying/ESD/silk/test points.
- Repo practices: OSHWA best practices + checklist; ship editable source (KiCad), not just PDFs; version
  HW (silk `fabric-hw vX.Y`) separately from FW (semver) with a compatibility matrix; commit the address
  map, port map, power budget, bus-ownership table, LA `.sr` captures, INA219 logs, BOM with MPNs; dual
  license (CERN-OHL-S hw / MIT fw / CC-BY-SA docs).
- Sources: mikroBUS v2.0; Digilent Pmod 1.3.1; SparkFun Qwiic; OpenAMP RPMsg; Arm HIPC; TI SCBA004/SCLA011;
  NXP UM10204; Nexperia AN10441; TI SPNA061; ST AN1636; OSHWA best practices.
