# 47 — The perfboard build: the wiring sheet, and what it has to beat

Date: 2026-09-11. Status: **the targets are measured; the board is not built yet.**

The breadboard build works and is capped twice over. 8 MB of 48 usable because the decoder's enable
shares a wire with a chip select, and 13.8 MB/s because five jumper wires cannot carry an edge faster
than 37 ns per nibble. The perfboard fixes both, and this is the sheet to build it from.

Nine chips. **72 MB.** Everything here is keyed by PSRAM pin number, because that is the orientation
the bench asked for.

---

## 1. What goes where

### The five signals every chip shares

Every PSRAM on the board gets the same five wires. These are the bus.

| PSRAM pin | what it is | Teensy pad |
|---|---|---|
| **6** | SCLK | **53** |
| **5** | SIO0 | **52** |
| **2** | SIO1 | **49** |
| **3** | SIO2 | **50** |
| **7** | SIO3 | **54** |

**Pin 53 is the clock, not pin 52.** That is the one to get right and the easiest to get wrong. It is
verified twice from the Teensy core itself in document 44, once from the core's pin table and once from
the IOMUXC setup the hardware controller uses, because a pinout card read off a picture has already
been wrong here once.

### Power, at every chip

| PSRAM pin | | |
|---|---|---|
| **8** | VCC | 3.3 V |
| **4** | VSS | GND |

And a **100 nF** between pins 8 and 4 at each chip, with the shortest loop you can physically make.
The bench already has these fitted on the breadboard chips and they are not optional.

### The one wire that is different per chip

Pin 1 is CE#, the chip select, and it is the only wire that differs between chips.

| chip | pin 1 goes to |
|---|---|
| onboard chip A, on the Teensy's RAM footprint | Teensy pad **48**, already wired, leave it |
| onboard chip B, on the second footprint | decoder **Y0**, pin 15 |
| new chip 1 | decoder **Y1**, pin 14 |
| new chip 2 | decoder **Y2**, pin 13 |
| new chip 3 | decoder **Y3**, pin 12 |
| new chip 4 | decoder **Y4**, pin 11 |
| new chip 5 | decoder **Y5**, pin 10 |
| new chip 6 | decoder **Y6**, pin 9 |
| new chip 7 | decoder **Y7**, pin 7 |

Onboard chip B already has a flying wire on its pin 1, because that pad tore off during the first
build. Its far end moves from chip A's select to decoder Y0. **That is the only thing that has to be
un-done rather than added.**

### The 74LVC138A

| decoder pin | | goes to |
|---|---|---|
| 1 | A | Teensy **2** |
| 2 | B | Teensy **3** |
| 3 | C | Teensy **4** |
| **4** | **G2A, the enable** | **Teensy 5** |
| 5 | G2B | **GND** |
| 6 | G1 | **3.3 V** |
| 8 | GND | GND |
| 16 | VCC | 3.3 V |
| 7, 9–15 | Y7 … Y0 | the chip selects above |

**Teensy pin 5 is the whole repair.** On the breadboard the enable is tied to pad 48, so asserting the
onboard chip's select also enables the decoder and two chips drive the data lines together. Giving the
enable a pin of its own removes the cap, and pin 5 is the right pin for two reasons: the core says it
is free, and it is in GPIO9 — the same register as the clock and the four data lines, which the driver
writes whole once per clock edge. A select in any other register would cost a second store on every
burst boundary. Pin 33 is the alternative if pin 5 is awkward; it is GPIO9 bit 7.

**G1 high and G2B low are not decoration.** The outputs are active only when G1 is high *and* both G2
inputs are low. Tie G1 to 3.3 V and G2B to ground, and drive G2A. Putting the enable on G1 instead
would hold the decoder off for ever, and the bring-up sketch says so by name if nothing answers.

With a dedicated enable, parking no longer needs a spare output: "nothing selected" is just the enable
high. So all eight decoder outputs carry a chip, instead of seven plus an unconnected one.

---

## 2. The part that decides whether this was worth building

Everything above is connectivity and it will work. The reason to build a perfboard at all is the
**ground return**, and it is the one thing a breadboard cannot give.

| | |
|---|---|
| what the breadboard needs | **37.2 to 40.3 ns per nibble** |
| the slowest FlexSPI2 can be clocked | **20.2 ns per nibble** |
| so the controller is | **1.85× too fast for that wiring, with no setting that works** |

That is why every controller clock sweep came back silent and why the bus is hand-driven. It was never
a software problem. And it is not the pad drive either: `psram_pads` swept drive strength, slew rate and
input hysteresis, eight combinations, and **every one gave the same 14.53 MB/s.** The wiring is the
limit, measured rather than assumed.

So, in order of how much each is worth:

1. **A ground wire running beside the five bus signals, the whole length of the chain, bonded to the
   Teensy ground at the Teensy end and again at the far end.** Every switching data line needs a return
   path. On a breadboard the return goes the long way round through the power rails, and that loop is
   what sets 37 ns. This is the change that buys the factor of two.
2. **Keep it a daisy chain, not a star.** One run past each chip in turn, with each chip's stub off the
   main run under about 15 mm. A star makes every branch a reflection.
3. **Shortest total run to the furthest chip.** This one is close to linear, so every centimetre saved
   is real.
4. **10 µF bulk at the 3.3 V entry to the chip group, and another at the far end.** The 100 nF at each
   chip handles the fast edge; the bulk handles the group. This is worth fitting and is *not* what
   limits the speed — at a failing edge rate, changing burst length from 4 to 32 bytes and adding idle
   gaps from 0 to 50 µs moved the error count only between 666 and 691 of 1024. Charge is not the
   problem. Signal return is.

**Even if it does not reach 20.2 ns, every nanosecond is linear throughput.** Going from 40 to 30 ns is
a third more read bandwidth with no other change.

---

## 3. What to flash, and what the output means

Flash `firmware/bench-one/tests/psram_perfboard/` first, before anything else. It assumes nothing about
what got soldered, and it has already been run end to end against the current breadboard so its own
logic is not in question.

It does six things and then keeps going:

| stage | what it answers |
|---|---|
| 1 | which of the ten possible chip selects has a chip on it, by identity command at 250 kHz |
| 2 | are those banks separate chips, or does one answer on several selects |
| 3 | each bank's own clean timing window, and **nanoseconds per nibble against 20.2** |
| 4 | chip select low per burst against the 8 µs refresh limit, and the burst chosen to fit it |
| 5 | the whole flat address space across every bank, written and verified |
| 6 | then soaks every bank indefinitely, printing the error-rate bound as it falls |

What to look for, in order:

- **Stage 1 should list nine chips.** If no decoder bank answers, the enable wire is the suspect and the
  sketch names it. If nothing at all answers, it prints the four causes in order of likelihood, with
  pin 53 first.
- **Stage 3's ns/nibble column is the number this board was built for.** Under 20.2 and the hardware
  controller becomes reachable, which brings memory mapping and about 33 MB/s. Then run
  `psram_clock_sweep`.
- **Stage 4 choosing a burst shorter than 96 bytes is normal and correct.** The timing and the refresh
  limit trade against each other: stepping one setting slower for timing margin lengthens a burst, and
  on this bench that took chip select low from 6.93 µs to 7.71 and produced one wrong byte in 8 MB.
  Shortening the burst to 80 took it to 6.52 µs and zero. The sketch does that arithmetic per bank.
- **Stage 6's bound is the qualification.** A bank with no errors yet is bounded, not proven. Leave it
  running; on the breadboard the bound reached 0.7 per billion after 1.3 GB per configuration.

---

## 4. What 72 MB changes

Capacity is not speed, and for this project it decides something speed does not: whether a model has to
be split across boards at all.

| | |
|---|---|
| a decoder layer at hidden size 512, Q4_K | 2.304 MB |
| layers that fit in 8 MB | 3 |
| layers that fit in **72 MB** | **31** |

A 24-layer model at hidden size 512 is 55 MB of weights. **That fits on one Teensy with room over.**
The pipeline, the hop cost, the vocabulary split and the load balancing all exist because models did
not fit on one node — and for this size, after this board, they do. Distribution becomes a throughput
choice rather than a necessity.

At hidden size 1024 a layer is 9 MB, so eight fit and splitting is still required. The boundary moves;
it does not vanish.

---

## 5. One thing the controller still cannot do, and why

If the perfboard gets under 20.2 ns, FlexSPI2 becomes usable — **for the chip on pad 48 only.**

The controller asserts its own chip select as part of a transfer, and it has exactly two: pad 48 and
pad 51. Pad 51 is the one that tore off. So the controller can reach onboard chip A and nothing else,
while the eight decoder banks stay on the hand-driven driver.

Making the controller reach all eight would mean putting the decoder enable on pad 48 and taking chip
A's CE# off it, which means lifting a pin on a chip already soldered to the Teensy. That is the upgrade
path, not the build. The discovery sketch needs no change to find that arrangement if it ever happens:
chip A would simply turn up as a decoder bank instead of on CS0.

So the honest expectation for the finished board is **8 MB at controller speed and 64 MB at bit-bang
speed**, with the bit-bang speed itself improved by however much the ground return buys.
