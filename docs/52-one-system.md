# 52 — One system: what each part is for, and why

*Every role below is assigned from a measurement taken on this hardware, except where marked. The point
of this document is that no part is here because it was in the box.*

## The principle that turns a pile into a machine

Every node is an independent processor, so **they all compute at once and a pass costs the slowest node,
not the sum of them all.** That single fact decides everything else:

> A node should hold bytes in proportion to how fast it can read them, not in proportion to how many it
> can hold.

Give every node an equal share and the fleet runs at the speed of its worst member. This project already
measured that on the render side — equal rows wasted a third of the machine — and it arrives here from a
different direction with the same answer.

Only partial sums cross the wires. A few bytes per node per pass, because each node computes on the
weights it physically holds. That is why a 1 Mbaud UART is enough to bind fifteen nodes together, and
why the link speed never appears in any of the arithmetic below.

## The tiers, by measured read rate

| tier | part | count | capacity each | MB/s of weights | source |
|---|---|---|---|---|---|
| fast | Luckfox DDR2 + NEON | 5 | ~14 MB usable | **81.0** | measured |
| middle | ESP32-S3 on-module PSRAM | 6 | 8 MB | *unmeasured* | — |
| capacity | Teensy, 2 chips, quad bus | 8 | 16 MB | **~11** | measured |
| reserve | Teensy, 8 chips, mixed bus | 1 | 64 MB | **3.9** | measured |

The spread between the top and bottom of that table is **21 to 1**. That ratio is the whole design
problem, and ignoring it is how you get a machine that runs at 3.9 MB/s no matter what else is plugged
in.

## What each part is actually for

**Luckfox — the fast tier, and one of them is the brain.** 81 MB/s and 648 MMAC/s measured, twenty-one
times a Teensy PSRAM bank. These hold the hottest weights in the model. One of the five also runs the
show: it has the operating system, the qualification sweeps, the logging and the archive, because those
are decisions and decisions belong on the board that can change without a reflash.

**Teensy — the capacity reserve, and the fabric.** Slow per byte and enormous per dollar. It is also the
*only* part here with the deterministic timing to drive a bit-banged PSRAM bus at all: this project
established that half a millisecond of wifi jitter is fatal to an 8 µs refresh window, which is why the
PSRAM hangs off Teensys and not off ESP32s. And it is the only part with enough spare UARTs to relay the
chain *and* host a Luckfox, so it is the fabric as well as the memory.

**ESP32-S3 — the middle tier.** Its PSRAM is reached through the chip's own memory controller rather
than a hand-timed bus, so the jitter rule does not apply to it and it can be trusted with weights. It
gets a different kernel from everything else because it has different silicon: no SMLAD, but 512 kB of
internal SRAM, which is enough to hold a table of every possible weight byte and turn eight
multiply-accumulates into one load and one add. **Its rate has not been measured** and every projection
below that involves it is marked.

**ATtiny85 — not compute.** 8 kB of flash and 512 bytes of RAM holds no useful slice of a model and
never will. Its job is the FPGA: hold a bitstream and clock it in at power-up, and sequence the rails.
That is a real job and it is the one they were bought for.

**FPGA, when it lands — the matrix engine.** This project's own numbers say why: a microcontroller
cannot move weights fast enough, and the gap is the argument.

## Wiring

### The chain, node to node

Upstream is Serial2 on pins 7 and 8. Downstream is Serial1 on pins 0 and 1.

| from node N | to node N+1 |
|---|---|
| pin 1 (TX1) | pin 7 (RX2) |
| pin 0 (RX1) | pin 8 (TX2) |
| GND | GND |

### A Luckfox leaf on each Teensy

A Luckfox exposes exactly one spare UART — UART3, because UART2 is its console — so it can be the end of
a link but never a relay. It is a leaf.

| Teensy | Luckfox Pico Mini B | Luckfox Pico (40-pin) |
|---|---|---|
| pin 14 (TX3) → | pin 13 · UART3_RX_M1 | pin 20 |
| pin 15 (RX3) ← | pin 12 · UART3_TX_M1 | pin 19 |
| GND | pin 2 or 21 | 18 or 23 |

**100 Ω in series on each signal line.** The Luckfox does this on its own console lines and this project
has been bitten by leaving it out.

Serial3 is pins 15 and 14 and, unlike Serial1, has no alternate pin pair in the Teensy core — so it can
never be remapped onto the PSRAM bus. Serial1's alternate pair is 52 and 53, which *are* PSRAM SIO0 and
SCLK. Never call `Serial1.setRX`.

### The ESP32-S3s

Same chain protocol, at the tail. Upstream on Serial1 (GPIO 18 RX, 17 TX), downstream on Serial2
(GPIO 16 RX, 15 TX). Those four are clear of the strapping pins, clear of USB on GPIO 19/20, and clear of
the octal PSRAM on GPIO 33–37.

### Order

    head Luckfox ─ T1(8 chips) ─ T2 ─ T3 ─ … ─ T9 ─ ESP1 ─ … ─ ESP6
                        │        │    │
                       Lk2      Lk3  Lk4  … (leaves, one per Teensy)

The head Luckfox is the one you plug into. Everything else is reached by address.

### Power

Estimated, not measured: nine Teensys at roughly 150 mA (the 8-chip head nearer 250), six S3s at roughly
100 mA, five Luckfoxes at roughly 250 mA. That is about **3.3 A at 5 V, call it 20 W**, so a real bench
supply rather than a USB port.

Star ground. Bulk 220–470 µF at each ESP32. And **cut the VUSB–VIN pad on every Teensy that has both
external 5 V and a USB cable attached**, or the bench supply back-feeds your USB host.

## What it adds up to

Capacity, at one bit a weight:

| | MB | parameters |
|---|---|---|
| 5 Luckfoxes | 70 | 560 M |
| 8 Teensys, 2 chips | 128 | 1.02 B |
| 1 Teensy, 8 chips | 64 | 512 M |
| 6 ESP32-S3 | 48 | 384 M |
| **total** | **310 MB** | **2.48 billion** |

But capacity is not the interesting number, because the pass costs the slowest node. Spread the model by
bandwidth instead and ask how big a model runs at a given speed:

| seconds per token | model that fits in that time |
|---|---|
| 1 s | ~210 MB = **1.68 B parameters** |
| 2 s | ~254 MB = 2.03 B |
| 16 s | 310 MB = 2.48 B (everything, capacity-bound) |

*The ESP32 contribution in those rows assumes 40 MB/s, which is a guess and is the one number here that
has not been measured. Everything else is from hardware.*

The 16-second row is what happens if the model is spread evenly: the 8-chip Teensy's 64 MB at 3.9 MB/s
takes 16.4 seconds and every other node waits for it. The 1-second row is the same hardware with the
weights placed by speed. **Same parts, sixteen times the throughput, and the only difference is which
bytes live where.**

That is the integration. Not fifteen nodes that each do something, but one allocation rule applied to
fifteen nodes that are honest about how fast they are.
