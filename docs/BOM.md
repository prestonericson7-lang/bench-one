# BOM

---

## Processors and core (owned)

- Luckfox Pico Mini B **×2** (RV1103) · Teensy 4.1 **×2** · ESP32-S3 (N16R8)
- **E32R40T** 4.0in display board — ESP32-**WROOM-32E**, ST7796 480×320, XPT2046 touch.
  Not an S3. No pin headers; 1.25 mm connectors only.
- 2.4in display on Teensy 1 — **confirmed TFT**. Driver probes the chip id at boot and warns if
  it disagrees with what was compiled. Uses PJRC ILI9341_t3, not Adafruit: Adafruit 1.6.3
  silently defaults to 8 MHz on a Teensy 4.1 (a 153 ms full repaint) and has no DMA path
  compiled for this processor at all
- Bambu A1 mini + PLA (BENCH-TOWER enclosure, built)

## Logic fabric (owned)

The bulk parts are abundant. **The bottleneck is the glue**, and it is not where the order
suggests.

| Part | Have | Notes |
|---|---|---|
| MCP23017 | 12 | 8 per I²C bus max (0x20–0x27) |
| 74HC138 | 24 | Chip-select decode, and a second one steers the mux tree |
| 74HC595 | 24 | Chip 0 is the fabric control byte |
| 74HC165 | 19 | Snapshot inputs |
| 74HC373 | 15 | Also the MISO gate — LE tied high makes it a tri-state buffer |
| 74HC4051 | 10 | Analog leaves |
| 74HC04 | 24 | Polarity flip, clock fan-out |
| **74HC14** | **4** | **Scarce.** The correct part for every debounce and slow edge |
| **74HC00 / 08 / 32** | **4 each** | **Scarce.** Across *both* kits combined, not per kit |
| 74HC164 | 4 | No output latch — never on a chip select or latch enable |

- 3× TXB0108 + 3× TXS0108 — **currently unused, and that is correct.** The whole fabric is 3.3 V.
  Reserve them for genuine 5 V modules.
- 1N4148 ×20 · full passive assortment

### The 74LS parts

**Unusable at 3.3 V, and the reason is the output side, not the input side.** TI specifies
74LS00 output-high at minimum 2.5 V with a 4.75 V supply — a guaranteed 2.25 V drop below rail.
At 3.3 V the same emitter-follower yields roughly **1.05 V worst case**, far under the 2.31 V
that both 74HC and the i.MX RT1062 require. And 74LS is not specified below 4.75 V at all.

Run them at 5 V instead and the output-high typical of 3.4 V already exceeds the Teensy 4.1's
3.6 V absolute maximum, driving current into its ESD clamp.

**One exception: the 74LS47.** Its outputs are open collector, rated 15 V off-state and 24 mA
on-state. Powered from 5 V it accepts a 3.3 V drive (V<sub>IH</sub> 2.0 V) and never presents a
5 V level back to the fabric. It is the only 74LS part in the kit with a legitimate job — driving
a 7-segment display. If there is no 7-segment display in the build, bag the LS47s with the rest.

## Instrumentation (owned)

- Lonely Binary 8-channel 24 MHz logic analyser — **~8 MSa/s per channel with all 8 live**, so
  SPI above about 4 MHz cannot be honestly decoded while capturing
- INA219 ×5 — bus range is **0–26 V**, not 32 V; 32 V is only the ADC's full-scale scaling
- AR9271 USB WiFi + USB-C OTG adapter — see the warning below
- CH340 USB-TTL ×5 — one is the Luckfox console lifeline, wire it before anything else

---

## To buy — for the fabric

**Revised after the verification pass. Full reasoning in
[07-parts-and-ports-findings.md](07-parts-and-ports-findings.md).**

The framing finding first: **the 74HC family is specified nowhere at 3.3 V.** The datasheet
columns are 2.0, 4.5 and 6.0 V. At 4.5 V an HC bus driver guarantees ±6 mA; at 3.3 V it
guarantees *nothing*. The whole fabric runs unspecified. It works, and no number backs it. The
Tier 1 list below closes that gap where it actually bites.

**Tier 1, about $45 total.**

| Part | Qty | Unblocks |
|---|---|---|
| **SN74LVC245AN** PDIP-20 | 6 | **The single best purchase.** The only DIP part that is both specified at 3.3 V with real drive (24 mA, 6.3 ns) *and* 5.5 V-tolerant on its inputs at 3.3 V VCC. That tolerance is the actual enabler for "any module plugs in": most breakouts are 5 V and the Teensy dies above 3.6 V. Buy this instead of the SN74HC245N |
| **SN74LV4051AN** PDIP-16 | 10 | **Buy now or never.** A properly 3.3 V-specified 8:1 analog mux, drop-in for the 74HC4051 — same package, same pinout. 190 Ω max over temperature where the HC part has *dashes* in its low-voltage column. TI marks it NRND and out of stock; this is a last-time-buy from distributor stock |
| **SN74AHC541N** PDIP-20 | 4 | Clock fan-out past ~8 chips. Buy the '541 not a '245: it is **unidirectional**, so there is no DIR pin to float at power-up and turn the buffer around into a 3.3 V-max Teensy pin |
| **MP1584 buck, ≥1.5 A** | 2 | A dedicated 3.3 V rail. **Verify the trimpot output on a meter before connecting anything.** An AMS1117 tops out near 400 mA and its dropout makes a sagging USB rail marginal |
| **100 Ω resistors** | 20 | Series on both Luckfox link wires, and on every '138 chip-select output. Not optional on the Luckfox link — see below |
| **74HC14** | 8 | You have four. Every debounce and every slow edge wants one, and stack 2 needs its own |
| **74HC00 / 08 / 32** | 8 each | Same reason. Four each across both kits is not enough for one stack, let alone two |
| **SN74HC573N** | 6 | Straight-through pinout version of the '373. Far easier to route before any PCB |
| **MCP6002 or TLV2372** | 4 | Rail-to-rail buffer for 4051 sources above ~1 kΩ, and mandatory for the Luckfox ADC divider |
| **TBD62083APG** P-DIP18 | 4 | Sink driver, only if anything behind a '595 draws over ~20 mA. **TPL7407LA does not exist in DIP** — SOIC and TSSOP only. This Toshiba DMOS array is pin-for-pin identical to a ULN2803A and far better on the numbers |
| ~~TCA9548A~~ | **0** | **Not needed, and the reason is better than "not yet".** The collision has already happened — twelve MCP23017 cannot fit eight addresses. But the Teensy 4.1 has **three hardware I²C buses**, giving 24 addresses for free. Two are already kept clear in the pin map |

## To buy — only if you build the discrete CPU

You own **zero** of every one of these, and their absence is what makes the current box an IO
expansion kit rather than a computer kit. See `ic-experiments.md`.

| Part | Why it blocks everything |
|---|---|
| **74HC86** (XOR) | No adder carry, no LFSR, no parity, no comparator. Four NANDs per gate makes a real datapath impossible |
| **74HC74** (edge-triggered D) | The '373 is level-sensitive. CPU registers race their own clock without edge triggering |
| **74HC283** (4-bit adder) | No ALU |
| **74HC161/163** (counter) | No program counter |
| **62256** SRAM | No memory |
| **28C64** EEPROM | No microcode store |

## To buy — for the second stack

- **2× PJRC Ethernet Kit** for the Teensy 4.1s. This is the right stack-to-stack link:
  94 Mbit/s measured, ~650 µs round trip, Auto-MDIX so straight CAT5 works — and it carries PTP
  at **σ = 40.7 ns** between two Teensys.
- **Not** USB between the two Luckfoxes. Both USB-C ports are hardwired as sinks (5.1 kΩ on
  CC1/CC2, VBUS feeds the system rail), so two boards cannot enumerate over a C-to-C cable at all.

---

## Verify from stock, do not buy

- 2.2 kΩ ×2 — I²C pull-ups, **once, at the master**
- 4.7 kΩ ×1 — the shared expander interrupt pull-up
- 10 kΩ — address straps, /OE pull-ups, the chip-select-enable pull-**down**
- 100 nF X7R — one per IC, within 10 mm of the pin
- 10–100 µF per fabric section · 220–470 µF at the ESP32 for transmit brownout

---

## Two warnings that cost hardware

**Put 100 Ω in series on both Luckfox link wires.** If the Teensy is powered while the Luckfox is
not, it drives 3.3 V into a pad whose supply rail is dead, and current flows through the pad's ESD
diode into an unpowered VCCIO6. That can latch or slowly degrade the SoC, and it looks like
nothing at all until the board stops booting. Luckfox fit 100 Ω on their own console lines for
exactly this reason.

**The AR9271 is not a plug-in.** It needs a kernel rebuild — the stock Luckfox defconfig has no
CFG80211, MAC80211 or ATH9K_HTC at all — plus a USB `dr_mode` change that permanently kills the
RNDIS management link. There is one USB port on the RV1103. Have the UART2 console working on
pins 4/5 before you start, because there is no Ethernet to fall back on (the microSD slot is storage, not a way in). And the
USB-C port never sources 5 V (5.1 kΩ Rd on both CC lines, no VBUS switch), so a passive OTG
adapter enumerates nothing — you need a powered hub or an external 5 V feed.
