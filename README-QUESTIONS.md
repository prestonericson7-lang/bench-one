# Questions and concerns — read this first

Written overnight. Everything here is either a **hard spec I need from you** or a **concern I am
flagging rather than hiding**. Nothing blocked the build: where a decision was needed I made it,
wrote it down, and marked how to reverse it.

**All four sketches compile.** The Python codec runs and passes its self-test, including your CRC
check value `0x6F91`. Nothing has touched hardware.

---

## What I need from you

### ~~Q1 — TFT or OLED?~~ ANSWERED: TFT

Built. It is on SPI0 with the fabric on SPI1, so display refreshes and fabric transactions never
queue behind each other.

One thing I did not assume: **the driver identifies the controller at runtime.** Your BOM says
ST7789, but most 2.4 inch 240×320 SPI modules are actually ILI9341 — ST7789 is far more common on
1.3 and 2.0 inch panels. Both are plausible and the wrong driver does not fail cleanly; it gives
a shifted image or wrong colours with no error.

So it reads the controller's ID register and picks. If your module does not bring MISO out — many
cheap ones do not — it falls back to ILI9341 and prints `ASSUMED` rather than `detected`. That
distinction is on the screen and on the console, because if the image ever comes up shifted,
knowing whether the controller was *read* or *guessed* is the difference between suspecting the
driver and suspecting the wiring.

### ~~Q2 — What plugs into the fabric?~~ ANSWERED: my call, and it is made

Full specification in **[docs/04-port-map.md](docs/04-port-map.md)**.

I did not invent modules you do not own. Instead every port is **a shape with a resource behind
it**, so whatever you plug in later already has what it needs.

The design decision worth knowing: a module needs more than the bus and a chip select — it needs a
reset, or a chip enable, or an interrupt. Those auxiliaries live on **different silicon depending
on which direction they point.**

| | MCP23017 pin | 74HC595 output |
|---|---|---|
| Direction | in or out | out only |
| Interrupt on change | yes, in hardware | no |
| Time to change one bit | ~120 µs | **~2 µs** |

So module *outputs* (interrupt, data-ready, busy) go to the expander, which is the only thing that
can raise a flag without being polled. Module *inputs* (reset, chip enable) come from the shift
chain, which is sixty times faster. Backwards is not a style error: a reset on an expander takes
120 µs, and an interrupt on a shift register is invisible until something polls for it.

Each of the eight SPI ports therefore gets one interrupt pin and one control pin, and the
arithmetic lands exactly: **eight ports × one interrupt = sixteen expander pins = one MCP23017**,
with its second bank held ready for the modules that have two interrupts.

Both are reachable by name with no new firmware:

```bash
./benchctl.py reset P3     # pulse a module's reset
./benchctl.py irq P3       # read its interrupt line
./benchctl.py panel        # walk the whole bring-up jig
```

There is also a **bring-up jig built entirely from parts you already own** — LEDs, buttons, two
jumpers — that proves the shift chains, the loopback, the ADC rails and the expander before any
module exists.

### Q3 — Any 5 V modules at all?

The fabric is 3.3 V throughout and needs no shifters, so your six TXB/TXS boards are unused —
which is the correct outcome, not an oversight. If something 5 V is coming, tell me which bus:
I²C or any pulled bus must use a **TXS**, push-pull SPI/UART must use a **TXB**, and backwards
gives you a bus that half-works.

### Q4 — What should Teensy 2 actually compute?

Job dispatch, cancellation, progress events and result paging are built. Seven benchmark jobs
exist so you can stress-test today. The *application* job types are empty because I do not know
what you want crunched. FFT? Correlation on captures? Pattern matching?

### Q5 — How long is the I²C run, and how many connectors?

The under-400 pF budget is generous, but ribbon runs 50–70 pF per metre per conductor and every
connector adds more. If the furthest MCP23017 is over about 30 cm away, the pull-ups want to drop
from 2.2 kΩ to 1.5 kΩ.

### Q6 — Does stack 2 need the same fabric?

If yes, **you are short before you start.** 74HC14, 00, 08, 32 and 164 exist only in the kits at
four pieces each across *both* boxes combined. One stack uses most of that.

---

## Answered while you slept

### The E32R40T connector — and the obvious answer was the wrong one

You said not to guess. I did not; the vendor's schematic settled it.

**Use P4, silkscreened `I2C`. Not P2, silkscreened `UART`.**

| P4 pin | Signal | Teensy 4.1 #1 |
|---|---|---|
| 1 | 3.3 V | leave unconnected |
| 2 | GPIO32 | pin 29 (TX7) |
| 3 | GPIO25 | pin 28 (RX7) |
| 4 | GND | any GND |

P2 is UART0 wired in parallel with the onboard CH340C through **100 Ω series resistors**. With USB
plugged in the CH340 drives that net hard while a Teensy reaches it through 100 Ω, so a Teensy
pulling low only gets the ESP32's input down to about 2.2 V — still a logic high. The link looks
completely dead, *only when USB is connected*, with nothing wrong anywhere you would look. P2 pin
1 is also a bidirectional 5 V node through a MOSFET body diode; never bond it to a Teensy rail.

Two practical points: the connectors are **1.25 mm pitch**, so JST-PH 2.0 mm will not mate. And
**do not remove R33/R34**, the 10 kΩ pull-ups on GPIO25/32 — the vendor says to remove them for
generic IO, but for a UART they hold both lines idle-high before `begin()` runs and while the
cable is unplugged.

Your free-pin list also missed two: GPIO32 (P4) and GPIO21 (P3) are both free. And the LCD's reset
is not a GPIO at all — it is tied to EN, shared with the ESP32's own reset.

### The fabric got its own SPI bus, which my first draft said was impossible

I wrote that SPI1 was unusable because its MISO is pin 1 — your tested radio's TX. Wrong. SPI1 has
an **alternate MISO on pin 39**, selected with `SPI1.setMISO(39)` before `begin()`. Pins 26, 27
and 39 are all free and all top-side.

So the display owns SPI0 (11/12/13) and the fabric owns SPI1 (26/27/39). Display refreshes and
fabric transactions stop queueing behind each other, and the fabric's clock is no longer the pin
with the onboard LED hanging off it.

**If you take one thing from this file:** miss that `setMISO` call and SPI1 claims pin 1, and the
radio link goes silent with no error anywhere.

### The display library problem, solved without touching your environment

Arduino_GFX 1.6.7 — which your own E32R40T firmware uses — **does not build against your installed
esp32 core 3.0.7.** It guards `ESP_INTR_CPU_AFFINITY_AUTO` on `ESP_ARDUINO_VERSION_MAJOR < 3`, but
that symbol only arrives in arduino-esp32 **3.1**. Your core is major 3, so the guard lets the
code through to a symbol that does not exist. The library's version check is too coarse by one
digit. **Your existing firmware for that board will not build either.**

I did not bump your core, because 3.0.7 is what your tested ESP32-S3 radio baseline was measured
on and arduino-cli holds one version per core. Instead the HMI uses **TFT_eSPI**, configured by a
sketch-local `build_opt.h` — no installed library edited, no other project affected. It also
drives the panel and the touch controller through one bus manager, which removes the shared-HSPI
arbitration problem your bring-up had to solve by hand.

---

## Concerns

### C1 — I corrected my own chip-select timing twice, and the number survived both times

First draft: 100 ns, interpolated from typical figures. Second: 500 ns, justified by TI's 2.0 V
column on the grounds that **no 3.3 V-relevant guarantee existed anywhere.** A fact-check killed
that too — **onsemi publishes a full guaranteed 3.0 V column** for the MC74HC138A: 125 ns
address-to-output at 85 °C, not 225 ns.

500 ns stands, now as 4× a real guaranteed bound instead of an arbitrary over-budget. **Replace it
with a measured value from your own capture** — that measurement is why the budget is written
down.

### C2 — There is no 3.3 V specification for the 74HC4051 at all

Not a gap in my notes. Neither Nexperia nor TI characterises on-resistance, turn-on or turn-off at
3.3 V — only at 2.0, 4.5, 6.0 and 9.0 V. **Every "70 Ω at 3.3 V" figure in circulation is
somebody's measurement**, usually derived from the 4.5 V column and 3–4× optimistic.

The firmware designs against the 2.0 V bound: Ron 300 Ω, ton 350 ns. Charge injection is not
specified by either vendor either.

Three consequences already in the code:

- **The bus sets settling, not the ADC.** Eight bussed Z pins are ~200 pF against a 2 pF sample
  capacitor. The 20 µs wait is sized for a 10 kΩ source.
- **Break-before-make applies to the mux tree too.** The '4051's own guarantee covers channels
  within one package, not between packages — two decoder outputs can be low together for a few
  nanoseconds, connecting two analog sources through ~500 Ω. The firmware clears the enable bit
  in its own latched write first.
- **The channel-to-pin map is non-monotonic**: Y0–Y7 are pins 13, 14, 15, 12, 1, 5, 2, 4. Wire
  them 1–8 in order and the mux works perfectly with scrambled channel numbers — which looks
  exactly like a firmware indexing bug.

### C3 — The 74HC165 will fight the bus, and you do not own the usual fix

A '165 output is a permanently-driving totem pole: no chip select, no tri-state. On a shared MISO
it fights whatever else drives that line — an estimated 17–40 mA per bit against a 25 mA per-pin
absolute maximum. That is a damage problem, not a data problem.

The standard fix is a 74HC125. You have none. You have fifteen 74HC373, and tying LE permanently
high makes one exactly that. Wire /OE to Teensy pin 4 with a 10 kΩ pull-up so it is Hi-Z at
power-up, and tie the seven unused D inputs to ground.

### C4 — The Luckfox link needs 100 Ω series resistors, and its margin is 90 mV

**If the Teensy is powered while the Luckfox is not**, it drives 3.3 V into a pad whose rail is
dead. Current flows through the ESD diode into an unpowered VCCIO6, which can latch or slowly
degrade the SoC — and it looks like nothing until the board stops booting. Luckfox fit 100 Ω on
their own console lines for this reason.

Separately: RV1103 output-high minimum is 2.40 V against the Teensy's 2.31 V input-high minimum.
**90 mV of worst-case margin.** It works. But framing errors that are marginal, temperature
dependent, or present in *one direction only* point here — not at the baud rate, which is exact
(0.000% error, genuinely).

Also **measure header pin 12 before wiring.** VCCIO6 is 1.8/3.3 V selectable *in silicon*,
strapped to 3.3 V on this revision by R20 with R21 not populated. That is a board fact, not a chip
fact.

### C5 — 8 MHz SPI on breadboard is a hope, not a specification

Silicon permits far more; unterminated jumpers and one clock fanning out to a dozen inputs do not.
Start at 8 MHz, prove it, then raise it deliberately and re-measure.

Note the instrument limit too: your analyser gives ~8 MSa/s per channel with all 8 live, so
**above about 4 MHz you cannot honestly decode while capturing.** There is a separate 2 MHz
capture clock in the firmware for this. Measuring at one speed and shipping at another without
saying so is how a "verified" timing claim becomes fiction.

### C6 — The self-test reports SKIPPED where it cannot see

Chip-select exclusivity and global deselect are the two most important things the fabric does, and
**nothing inside the Teensy can read the '138's outputs back.** The self-test walks all eight
addresses, pulses the marker pin so the analyser can prove it, and reports SKIPPED — not PASS. A
self-test that passes by not looking is worse than none, because it gets trusted.

### C7 — Nothing here has run on hardware

Every line is unbuilt and unrun except the Python codec, which passes its self-test and reproduces
your CRC check value exactly. The claims in the comments trace to datasheets, not to your bench.
Treat the whole thing as a well-argued hypothesis until the analyser says otherwise.

---

## Corrections to the project's own documents

**`docs/01-brain-and-radio.md` describes the v1 protocol, not what is on your boards.** It says
CRC-8 poly 0x31 check 0xA2 with a frame of `START|LEN_H|LEN_L|SEQ|CMD|PAYLOAD|CRC8`. Your firmware
is v2:

```
START(0xAA) | LEN_H | LEN_L | SEQ | FLAGS | CHAN | CMD | PAYLOAD | CRC_H | CRC_L
CRC-16/MCRF4XX, poly 0x1021 reflected to 0x8408, init 0xFFFF, check 0x6F91
SEQ bit 7 is a class bit: set = unsolicited. Not "SEQ 0x00 means unsolicited" — that was v1.
```

A decoder built from that summary fails on every frame and looks like a wiring fault. I have not
touched the doc because it is your published artifact.

**The handoff proposes 0x30–0x3F for worker jobs. That range is already the ESP-NOW mesh
commands** on channel 0x04. I used new channels instead — 0x05 fabric, 0x06 worker, 0x07
orchestrator, 0x08 display, 0x09 stack-to-stack — and left 0x40–0x4F deliberately empty so
anything written against the old plan fails loudly instead of landing on a real command.

---

## The two direction calls

**The discrete-logic CPU** — `docs/ic-experiments.md`, now with real arithmetic. Your entire
inventory holds 688 bits of state; the smallest Linux-capable RISC-V register file alone needs
992. My recommendation is a **bit-serial CPU, ~18 chips from stock**, wired as a fabric peripheral
the Luckfox can load and run programs on. PDP-8/S class, genuinely rare, and nobody has published
a homebrew CPU as an addressable device on a heterogeneous MCU stack.

**The GPU** — `docs/06-performance-and-scaling.md`. The blocker is not mechanical: the RV1103 has
no PCIe controller in the silicon. It also has no display controller, no GPU and no video decoder
at all — it is a camera SoC with a CPU attached. What it *does* have that a Pi 5 does not is a
hardware H.264/H.265 encoder and a 0.5 TOPS NPU. And the stack's real win is the time domain:
**18.33 ns from GPIO edge to ISR on the M7 against ~2 µs with 150 µs spikes on Linux** — roughly
8,000×, which a Pi 5 cannot buy at any price.

---

## Security note

The research ran 30 agents across two sessions. None of the fetched pages carried an injection
attempt this time. The earlier pass did hit fake `system-reminder` text on a datasheet mirror
trying to rewrite commit attribution, and it was correctly treated as page content rather than
instruction.

## 2026-09-29 — the machine (machine/README.md)

- [ ] **STM32H743 boards:** which vendor/board exactly? A photo of the top and bottom silkscreen, or the
      listing / schematic link. Needed for the SDMMC, FMC SDRAM, USB and USART pins before any firmware
      (machine/stm32/README.md). Listing seen: TKOWTB "STM32H743IIT6 Core Board", 55×85 mm, dual USB-C.
- [ ] **Model download:** go-ahead to fetch Qwen2.5-Coder-0.5B-Instruct GGUF q8_0 (~531 MB) from
      huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF for the engines, the Teensy and the STM32 cards;
      and whether the 1.5B (~1.1 GB) should come too.
- [ ] **Card #2 (32 GB):** put it in the PC's reader and say so; I write the machine image to it and hand it back.
