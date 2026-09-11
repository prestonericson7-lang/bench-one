# 03 — The Logic Fabric (where every IC goes, how it wires)

The expansion layer on BENCH ONE's breadboard bay. Principle: **minimal always-on core →
addressable layers → generic module ports**, so any radio/sensor drops in with clean control.
The fabric is a nervous system, **not compute** — it routes and expands IO; it does not think.

Owner: **Teensy1 (master)**. It exposes SPI0, I2C, 3 decoder-address lines + 1 enable, and
two shift-latch lines. Everything below hangs off those. Full datasheet backing +
sources: `fabric-research.md`.

---
## Layer A — I2C GPIO expansion (MCP23017)  → up to 128 IO on 2 wires
- Address 0x20–0x27 via A2/A1/A0 straps (10k to GND + jumper to 3V3; never float). 8 chips max/bus.
- **Pull-ups exactly once at the master: 2.2 kΩ** (1.5k if bus >150 pF). Keep total bus <400 pF.
  At 3.3 V you're capped at 400 kHz. Daisy-chain SDA/SCL, stubs <5 cm.
- INT: set IOCON MIRROR=1 + ODR=1 (open-drain), tie all INTs to ONE Teensy IRQ pin, 4.7k to 3V3.
  **ISR must read BOTH ports** (GPIOA+GPIOB) or interrupts wedge silently. Read INTCAP before GPIO.
- Sink-drive LEDs OK (anode→3V3 via ~270Ω, LOW=on, ≤150 mA/package); never source-drive (3 mA ceiling).
- 100 nF across pins 9↔10 at every chip; 10 µF bulk per module.
- Teensy1 has 3 I2C buses (Wire 18/19, Wire1 17/16, Wire2 25/24) → 24 chips / 384 IO with no mux.
  Past that: TCA9548A mux (also fixes address collisions with I2C radios that squat 0x20–0x27).
- **Budget (of ~12):** minimal 2 (0x20/0x21 = 32 IO). Full 8 = 128 IO. Hold ~4 spare.

## Layer B — SPI chip-select decode (74HC138)  → 8 CS from 4 pins
- A0/A1/A2 = Teensy 2/3/4; global enable /CS-gate = pin 33 (10k pull-DOWN — pins float Hi-Z at boot).
- E-law: outputs all HIGH unless E1=L·E2=L·E3=H, then one Yn LOW. **No "none" address → use the
  enable as global deselect.** Sequence always: gate off → set address → settle ~100 ns → gate on →
  transfer → gate off. **Break-before-make** or you walk a low pulse across other CS lines.
- Each Yn → 100 Ω series → module /CS, with **10k pull-up at each port** (safe hot-plug / power-off).
- Cascade: two '138 → 1-of-16 (enable = 4th address bit); a master '138 selecting leaves → 32/64.
  Budget ~200 ns for two tiers. Use 74HC, never HCT.
- **Budget (of ~20):** minimal 1 (8 CS). Full 1+8 = 64 CS from 7 pins; ~11 spare.
- 138 vs 595 for CS: 138 = fast/direct/atomic/limited pins; 595 = unlimited/serial/~µs per change.
  Hybrid: 138 owns CS, 595 handles slow RESET/power/config bits.

## Layer C — SPI shift IO (74HC595 out / 74HC165 in)  → bulk digital
- **595 (out):** SER←MOSI, SRCLK←SCK, RCLK←GPIO latch (RCLK is what makes output atomic — outputs
  hold through the shift, no glitch). /OE→3V3 via 10k (Hi-Z at boot) or PWM for global dim. /SRCLR→3V3.
  QH'→next SER. ~20 MHz @3.3V (run 4–10 on breadboard). Package total **70 mA** is the LED ceiling
  (8×4 mA fine; 8×20 mA kills it → ULN2803A / TPL7407LA for the 3.3V-correct part). First byte out
  lands in the FARThest chip → fill buffer in reverse chip order.
- **165 (in):** /PL←GPIO (LOW = simultaneous snapshot of all 8), CLK←SCK, QH→MISO, SER←prev QH,
  CLK_INH→GND. **H (pin 6) exits first → byte is bit7=H … bit0=A.** Every input pin gets a 10k
  pull (never float); 1k series for ESD; debounce mechanical inputs (10k+100nF + 3-sample firmware).
- **MISO contention:** the 165's QH always drives — don't share MISO with an SD card/sensor. Give the
  fabric its own SPI port (SPI1/SPI2) OR gate 165 QH through a 74HC125. Bring up ports one at a time.
- Both are SPI mode 0 → one SCK serves both (595 on MOSI, 165 on MISO). Chain limit is the shared
  SCK/RCLK/PL net; past ~8 chips buffer those lines with a 74HC541 (identically per chip).
- **Budget:** minimal 1×595 + 1×165. Full 20×595 = 160 out, 15×165 = 120 in.

## Layer D — Analog expansion (74HC4051 mux tree)  → 64 sensors on 1 ADC pin
- 8 leaves, all Z(3) bussed to one **Teensy ADC pin (0–3.3 V)**, S0/S1/S2 shared, a **74HC138**
  driving each leaf's /E(6) → 64:1 with 6 GPIO + 1 ADC pin. (One Ron in path — better than a
  master-mux tree's two.) **Tie VEE(7)→GND** (the #1 omission → unipolar 0–3.3 V).
- Ron ~150–300 Ω at 3.3 V and signal-dependent; leakage ±1 µA. Keep sensor source ≤1–10 kΩ or buffer
  with a rail-to-rail op-amp (MCP6002/TLV2372), else gain error. After switching S0–S2 wait
  5–20 µs and **discard the first reading** (charge injection).
- **ADC choice:** Teensy 4.1 (recommended, 0–3.3 V, 12-bit). ESP32-S3 nonlinear, top ~12% unusable,
  ADC2 dead with WiFi. **Luckfox SARADC = 1.8 V max, 10-bit** — buffer + divide (10k/12k → 1.80 V)
  and use **SARADC_IN1** (IN0 is a boot strap).
- **Budget (of ~10):** minimal 1 (8 ch). Full 8 leaves + 1×138 = 64 ch.

## Layer E — Glue (74HC373 latch / 74HC04 inv / gates / 74HC245)
- **74HC373**: transparent latch — parallel-bus demux (one 8-bit bus → N module ports, each latched
  by its own LE) and input snapshotting. /OE tri-states onto a shared bus; keep /OE exclusive across
  modules or you get shoot-through. LE is active-HIGH — invert a '138 (active-low) output with a
  74HC04 to drive LE. (Buy 74HC573 for straight-through pinout before a PCB — far easier routing.)
- **74HC04**: the load-bearing use is exactly that 138→373 polarity flip, plus clock fanout (2 gates
  to keep polarity). NOT a power buffer. Don't build an RC oscillator from it — use **74HC14** (Schmitt)
  for debounce/clean edges/clocks.
- **Bus transceiver:** need bidirectional shared data bus → **74HC245** (DIR/OE). His only '245 is a
  **74LS245 — do NOT use at 3.3 V.** Buy 74HC245 (or 74LVC245A if bridging to a 5 V island).
- Gates 74HC00/08/32 for strobe gating (e.g. /WE = /CS OR /STROBE). Kit has no HC02/86/74 → make
  from HC00 if needed.
- **Avoid every 74LS part at 3.3 V** (TTL thresholds, wrong VOH, 400 µA/input). HC only on this fabric.

## Level shifting (his 3× TXB0108 + 3× TXS0108)
- All-3.3 V inter-chip = **direct wire, no shifter.** Reserve all six for real 5 V modules.
- **I2C / any pulled bus → TXS0108 only** (one at the 5 V branch, not per device; remove/raise the
  4.7k pull-ups on the TXS side — it has internal ones). **SPI/UART/clock/push-pull → TXB0108**
  (never on I2C — it fights pull-ups). VCCA=3.3 low side, VCCB=5 high side, common GND, OE pulled to
  VCCA. Stubs <10 cm (auto-direction edges die on long/high-cap lines).
- **Verify Luckfox pad voltages with a meter first.** RV1103 has mixed 1.8/3.3 V IO banks; the UART3
  pads should idle 3.3 V but confirm. A 1.8 V pad crossing to 3.3 V fabric needs a shifter.

## Power & instrumentation
- 5 V rail **≥2.5 A** (typ ~1.2 A, peak ~1.5–1.8 A). USB banks may auto-off under ~50–100 mA → add a
  bleed load. 3.3 V via **MP1584 buck ≥1.5 A** (verify trimpot output on a meter BEFORE connecting;
  AMS1117 tops out ~400 mA and its dropout makes a sagging USB rail marginal).
- Decoupling: 100 nF X7R at every IC (<10 mm leads); 10 µF per fabric section; 22–100 µF at the buck;
  **220–470 µF + 10 µF at the ESP32** for TX brownout. Star ground at rail entry.
- **INA219** high-side (shunt in +rail): put the total-stack sensor on the **5 V side before the buck**
  (0.1 Ω shunt drops 150 mV @1.5 A). For the 100–300 mA fabric use PGA/1 (±400 mA). Addresses
  0x40–0x4F. Suggested: 5V-in, 3V3-fabric, ESP feed, Luckfox feed.
- **Logic analyzer** (24 MHz clone → ~8 MSa/s on 8 ch): cap SPI SCK at ~4 MHz while capturing; I2C
  100/400 k and UART ≤1 M fine. 0.1" tap header per bus, GND every 2–3 signals. Trigger: CS-fall
  (SPI), SDA-fall-while-SCL-high (I2C START), start bit (UART). Toggle a spare GPIO as a marker for
  intermittents.

## Generic module ports (the "drop in anything" spec)
Adopt proven port shapes so modules are keyed and unambiguous (see fabric-research.md for pinouts):
- **I2C sensors:** Qwiic/STEMMA-QT JST-SH (GND/3V3/SDA/SCL) — huge drop-in ecosystem, polarized.
- **SPI / analog / GPIO:** keyed 0.1" headers, power pins outermost, one bus per port, **series R +
  ESD at the host**, silk pin names on BOTH boards, and a port-type tag in silk (e.g. `P3-I2C`).
- Publish a **port map** (silk designator → net → MCU pin) and an **address map** in the repo.

## Bring-up checklist (gate each layer — this is the publishable evidence)
- **L0 unpowered:** continuity + rail↔GND shorts on every port; verify keying; ohm the pull-ups.
- **L1 power:** current-limited supply; measure inrush + quiescent with INA219 BEFORE firmware; log mA/module.
- **L2 bus idle:** LA the idle SDA/SCL both-high; measure actual I2C rise time vs the 400 pF limit,
  with 1 device then N (watch capacitance creep).
- **L3 one device at a time:** I2C scan with exactly one module seated; log address; add incrementally.
- **L4 loopback:** SPI MOSI→MISO jumper proves clocking/CS; 595→165 jumper proves the shift fabric.
- **L5 per-layer LA capture:** decode I2C/SPI, verify CS/latch edges + setup/hold; save `.sr` to the repo.
- **L6 power characterization:** INA219 per rail/port idle vs active vs fault; document max module budget.
- **L7 regression:** scripted self-test (scan + loopback + current) diffed against golden values.
