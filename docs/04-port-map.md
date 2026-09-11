# 04 — The port map

You said you do not know what plugs into the fabric and that deciding is my job. So this file
decides, and it decides in the only way that survives not knowing: **every port is a shape, not a
guess at a part.**

No module is named anywhere in this document. Naming one would put a fiction in the map. Instead
each port is specified so that when you plug something in, the thing it needs is already there.

---

## The idea in one paragraph

Eight SPI ports, four I²C ports, sixty-four analog channels, thirty-two general outputs and
thirty-two general inputs. Every one addressed by a silk label, every one backed by silicon chosen
for the *direction* of the signal it carries. That last part is the whole design: an interrupt
input and a reset output have opposite requirements, and giving them the same kind of pin wastes
one of them.

---

## Which silicon carries which signal, and why

A module needs more than SCK, MOSI, MISO and a chip select. It needs a reset, or a chip enable,
or an interrupt, or a data-ready. Those auxiliary signals are where a fabric either works or
becomes a pile of jumper wires.

The two candidate homes are not equivalent:

| | MCP23017 pin | 74HC595 output |
|---|---|---|
| Direction | input or output | **output only** |
| Interrupt on change | **yes**, hardware | no |
| Time to change one bit | ~120 µs (I²C transaction) | ~2 µs (SPI byte + latch) |
| Atomic with others | 16 bits per transaction | **entire chain, one RCLK edge** |

A 74HC595 output is roughly **sixty times faster** than an expander pin. An MCP23017 pin is the
only one that can tell you something happened without being asked.

So the assignment follows the direction of the signal:

- **Module outputs → into the MCP23017.** Interrupts, data-ready, busy flags. These are things
  the module tells you, and only the expander can raise a flag.
- **Module inputs → from the 74HC595.** Resets, chip enables, mode straps. These are things you
  tell the module, and the shift chain does it sixty times faster and latches atomically.

Getting this backwards is not a style error. Putting a reset line on an expander makes every reset
120 µs; putting an interrupt on a shift register makes it invisible until you poll for it.

### The signals that can live on neither

Some auxiliaries must change *within* an SPI transaction or with microsecond timing. Those get a
native Teensy pin or they do not work at all:

- **Display DC** changes between the command byte and the data bytes — per byte, at bus speed.
  It is on native pin 9 and it always will be.
- **nRF24L01+ CE** is separate from CSN and gates the radio state machine with roughly 10 µs
  granularity. On a 595 that is 2 µs and fine; on an expander it is 120 µs and the part will
  work only in its slower modes.

---

## SPI ports P0–P7

Eight of them, one per 74HC138 output. Each carries the bus, its own decoded chip select, one
interrupt input and one control output.

### Connector: 2×5 shrouded 0.1 inch IDC

Shrouded and notched, so it **cannot be inserted backwards.** That is worth the extra part: a
reversed 8-pin single-row header puts 3.3 V onto a signal pin, and the module usually does not
survive it.

```
        ┌───────────┐
   1  ──┤ GND   3V3 ├──  2
   3  ──┤ SCK  MOSI ├──  4
   5  ──┤ MISO  /CS ├──  6
   7  ──┤ IRQ   CTL ├──  8
   9  ──┤ GND   3V3 ├── 10
        └─────╥─────┘
            notch
```

| Pin | Signal | Source | Notes |
|---|---|---|---|
| 1, 9 | GND | — | Two grounds, one at each end. Every signal has a return beside it in the ribbon |
| 2, 10 | 3.3 V | — | Power outermost, so a partially-seated connector loses power before it shorts a signal |
| 3 | SCK | Teensy 27 (SPI1) | |
| 4 | MOSI | Teensy 26 (SPI1) | |
| 5 | MISO | Teensy 39 (SPI1) | |
| 6 | /CS | 74HC138 Yn, **100 Ω series** | 10 kΩ pull-up at the port, so an unpowered module is never selected |
| 7 | IRQ | MCP_A pin n | Interrupt-on-change, open-drain, wire-ORed |
| 8 | CTL | 74HC595 OA bit n | Reset, chip-enable, or mode strap |

### Port-to-resource map

| Port | 138 output | IRQ (MCP_A 0x20) | CTL (595 chip 1) | Second IRQ if needed |
|---|---|---|---|---|
| P0 | Y0 | GPA0 | OA.0 | GPB0 |
| P1 | Y1 | GPA1 | OA.1 | GPB1 |
| P2 | Y2 | GPA2 | OA.2 | GPB2 |
| P3 | Y3 | GPA3 | OA.3 | GPB3 |
| P4 | Y4 | GPA4 | OA.4 | GPB4 |
| P5 | Y5 | GPA5 | OA.5 | GPB5 |
| P6 | Y6 | GPA6 | OA.6 | GPB6 |
| P7 | Y7 | GPA7 | OA.7 | GPB7 |

**Sixteen expander pins is exactly one MCP23017**, which is why the count works out. Port GPA is
the primary interrupt; port GPB is a second one for the modules that have two — a CC1101 has GDO0
and GDO2, a LoRa module has DIO0 through DIO5.

P7 is reserved as the guaranteed-idle address. Parking the decoder there means "nothing selected"
is reachable even if the enable line is ever in doubt, which is what lets the deselect self-test
tell a stuck enable from a stuck address.

---

## I²C ports M0–M3

### Connector: JST-SH 4-pin, Qwiic / STEMMA QT order

```
   1  GND
   2  3.3 V
   3  SDA
   4  SCL
```

Polarised, so it cannot be reversed, and it opens a very large ecosystem of ready-made sensor
boards and cables. Buy the sockets and a handful of cables; both are cheap.

**For breadboard now**, the same four signals in the same order on 0.1 inch works fine. Keep the
order identical so that moving to the proper connector later is a mechanical change and not a
rewiring.

Pull-ups live **once, at the master**. Never on a port.

### Address space

`0x20`–`0x27` is reserved for the expander bank and must stay reserved. That block collides with
PCF8574 expanders and with a number of I²C radio modules — when something has to live there, it
goes behind a TCA9548A or onto Wire1 / Wire2, both of which are deliberately kept clear.

---

## Analog ports, 64 channels

Eight 74HC4051 leaves, eight channels each, all bussed to Teensy pin 41.

### Connector: 3-pin 0.1 inch — GND, 3.3 V, SIG

**Power in the middle, on purpose.** Reverse this connector and SIG lands where GND was and GND
where SIG was, while 3.3 V stays put. Nothing shorts. This is the same reasoning behind the servo
connector order, and it is the least-bad arrangement a 3-pin header allows.

Two things the firmware already handles so a port does not have to: the first conversion after a
channel change is discarded unconditionally, and the leaf enable is cleared in its own latched
write before the address changes.

Keep the sensor's source impedance at or below 10 kΩ, or buffer it. The mux's on-resistance is
in series with your signal and it is **not specified at 3.3 V by anyone** — design against 300 Ω.

---

## Bulk digital

| Silk | Backing | Width | For |
|---|---|---|---|
| `CTL` | 74HC595 chip 0 | 8 | **Reserved.** The fabric's own control byte — it steers the analog tree |
| `OA` | 74HC595 chip 1 | 8 | The eight SPI-port CTL lines |
| `OB` | 74HC595 chip 2 | 8 | General outputs. Sink-driven only |
| `IA` | 74HC165 chip 0 | 8 | Fast snapshot inputs — all captured on one edge |
| `IB` | 74HC165 chip 1 | 8 | More of the same |
| `XA` | MCP23017 0x20 | 16 | The SPI-port interrupts |
| `XB` | MCP23017 0x21 | 16 | Panel: 8 LEDs on GPA, 8 buttons on GPB |

### Connector for OB, IA, IB: 10-pin 0.1 inch

Eight signals plus a ground at each end. Same reversal logic as the SPI ports, without the shroud
because these carry nothing that a reversal destroys.

### Two rules that are electrical, not stylistic

**Every output on this fabric sinks, never sources.** An MCP23017 guarantees its levels at 8 mA
sinking but only 3 mA sourcing. Every LED goes anode to 3.3 V through its resistor, with the pin
pulling the cathode low.

**A 74HC595's package total is 70 mA**, not 70 mA per pin. Eight LEDs at 4 mA is fine. Eight at
20 mA destroys the chip. Anything above about 20 mA per channel needs a driver behind it.

---

## The bring-up jig — prove the fabric before any module exists

Built entirely from parts you already own. This is what turns the L0–L7 ladder from a checklist
into something you can actually run tonight.

| Fit | Where | Proves |
|---|---|---|
| 8 LEDs, anode to 3V3 via 270 Ω | `OB` outputs | The 595 chain, chain order, and the RCLK latch |
| 8 jumpers or buttons to GND, 10 kΩ pull-ups | `IA` inputs | The 165 chain, and the bit order (pin 6 is bit 7) |
| One wire, `OB` bit 0 → `IA` bit 0 | between them | The walking-1 loopback, which maps every bit position independently |
| One wire, MOSI → MISO at the header | SPI1 | Clocking, before any chip is involved |
| Leaf A0 channel 0 → GND, channel 7 → 3V3 | analog | The ADC rails, and that VEE is actually tied |
| 8 LEDs on `XB` GPA, 8 buttons on `XB` GPB | expander | I²C, interrupt-on-change, and the both-ports-read rule |
| Analyser on pins 30, 31, 32, 33 | 138 | Chip-select exclusivity — the one thing firmware **cannot** check itself |

That last row is the important one. Nothing inside the Teensy can read the decoder's outputs back,
which is why the self-test reports those two tests as SKIPPED rather than PASS. The marker pin
pulses once before each address is set and three times at the end of the walk, so the capture has
an unambiguous trigger.

---

## What is deliberately still empty

Every port above is a **shape with a resource behind it.** None of them names a device.

When you name a module, three things happen and none of them touch firmware you have already
flashed: it gets a line in the port table, a silk label on the board, and an entry in the address
map. If it needs a fast path, it gets a command in the 0x50–0x6F range.

Until then the raw SPI and raw I²C escape hatches already drive it from Python. That is the point
of building the shape first.
