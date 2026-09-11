# 38 — DDR3 on a microcontroller, and the three ceilings that decide it

Date: 2026-09-10. Status: **simulated and synthesised, never on hardware.** Every figure below is
either traced to a datasheet page or calculated from one, and says which. The one measured number
quoted, 39.3 MB/s of nibble unpacking, comes from [23](23-real-runtime-measured.md).

The goal was narrow: give one Teensy 4.1 a great deal more memory than the 16 MB of PSRAM it is rated
for, using a dead desktop DIMM and a small FPGA, **without the FPGA doing any of the work.** The
constraint matters more than it sounds. If the FPGA unpacks or accumulates anything, the resulting
throughput is a property of the pair and tells us nothing about what a Teensy can do. So the FPGA
moves bytes and nothing else, and the number that comes out is the microcontroller's own.

---

## The answer, first

**256 MB at 19.5 MB/s effective, which is 1.07× the PSRAM it replaces and sixteen times its
capacity** — and 5.4 MB/s, about a fifth of the PSRAM, in the configuration that can be built with
ordinary level translators. Not the dramatic speed win the idea suggests. The interesting results are
elsewhere: in finding exactly why it cannot be faster, and in discovering that the limit is neither
the DRAM nor the wiring.

Three ceilings stack. The lowest wins, and which one is lowest changes with configuration.

| ceiling | rate | source |
|---|---|---|
| The Teensy's external bus | **66.5 MB/s** | RT1060 datasheet table 38 |
| DDR3 at 25 MHz on 8 data lines | **50.0 MB/s** | calculated from BL8 and tCCD |
| The Teensy's own nibble unpacking | **39.3 MB/s** | measured, doc 23 |
| The PSRAM this replaces | 33.9 MB/s | measured |

Because read and unpack **add** on a CPU, effective throughput is `1/(1/read + 1/unpack)`. That rule
predicts the measured PSRAM result of 18.2 MB/s exactly, which is why it is trusted to forecast the
rest:

| configuration | raw read | effective | vs PSRAM |
|---|---|---|---|
| PSRAM, 16 MB | 33.9 | 18.2 | 1.00× |
| bridge, single-bit link, 6.25 MHz memory | 6.2 | 5.4 | 0.29× |
| bridge, four-line link, 25 MHz memory | 39.6 | 19.5 | 1.07× |
| four lines at the bus ceiling | 66.5 | 24.7 | 1.36× |
| infinitely fast link | — | 39.3 | 2.16× |

There is no row between the first two, and that is a finding rather than an omission: see the section
on the rate rule below.

**The last row is the finding.** Past about 66 MB/s the processor's own unpacking is the wall and more
memory bandwidth buys literally nothing. The project has been treating memory as the scarce resource;
for this node, above that threshold, it is not. The next real win is in the unpack kernel.

---

## Ceiling one: the bus, and why every escape route is closed

A Teensy 4.1's widest external memory path is FlexSPI2 port A: four data lines, on the bottom-side
pads. Pin 53 is the clock, 52 / 49 / 50 / 54 are the four data lines, 48 is the PSRAM's chip select
and **51 is a second chip select that is free on any board without a second PSRAM fitted.**

Per datasheet tables 38 and 42, with the read strobe looped back through the DQS pad — which is what
the Teensy core configures — the bus allows 133 MHz single rate or 66 MHz double rate. Four lines at
133 MHz is 66.5 MB/s either way, so **double data rate buys nothing here.** The core ships the clock
at 105.6 MHz, not the 88 MHz its commented-out lines suggest.

Three faster paths exist on the silicon and all are closed by bonding rather than by speed:

| path | would give | why not |
|---|---|---|
| Device-driven strobe, 166 MHz | 166 MB/s | needs FlexSPI2's DQS on pad EMC_23, not a Teensy 4.1 pin |
| The parallel memory controller | ~332 MB/s | needs 41 pads; 16 are bonded out |
| FlexSPI2's second port, combined | 133 MB/s | its data lines are pads EMC_13 to 16, absent from the core |

FlexSPI1's second port is tantalising — its data lines *are* on pins 16, 17, 40 and 41, and its
strobe on pin 23 — but that port's clock exists only on pad SD_B1_04, which is not bonded either, so
it cannot be clocked at all.

So 66.5 MB/s is not a tuning target, it is a property of the package. Worth writing down once so
nobody goes looking again.

---

## Ceiling two: DDR3 is specified to run slowly

The assumption that DDR3 needs hundreds of megahertz is simply wrong. It has a documented **DLL
disable mode**, and Micron's 2 Gb datasheet specifies `tCK(DLL_DIS)` from **8 ns to 7800 ns** — 125 MHz
down to 128 kHz. Running at 25 MHz sits in the middle of a published window.

Every timing quoted in nanoseconds is a **minimum**, so a slow clock satisfies tRCD, tRP, tRAS and
tRFC in one or two cycles and the numbers that make DDR3 hard stop mattering. One does not relax:
refresh is a wall-clock deadline of 8192 commands per 64 ms, and it is the only thing in the design
that constrains how long a single transfer may be.

The mode costs three things. Only CL=6 and CWL=6 exist. On-die termination is unavailable, so Rtt
must be programmed to zero and the ODT ball held low. And Micron states plainly that it does not
warrant normal-mode timings or functionality there — which is the honest shape of the experiment.

### Capacity: eight data lines reach one chip, not one stick

A DIMM rank is eight x8 chips sharing one address and command bus, each supplying eight of the
sixty-four data bits. Wire eight data lines and you are talking to **one chip**: 8 banks × 32768 rows
× 1 KB = **256 MB**. The other 1.75 GB of a 4 GB stick is behind the other fifty-six lines.

That is a near-perfect match to the hardware by coincidence: FlexSPI2's memory-mapped window is
**240 MB**, so one chip very nearly fills the aperture exactly. Going wider needs translators, not
address lines.

### 25 MHz is the ceiling for a single-edge design

Write data must sit a quarter period either side of the strobe edge that captures it. At a divider of
2 against a 100 MHz fabric clock, the quarter point collapses onto the edge itself — data changes
exactly when the device samples — and writes fail. Verified, not assumed: that configuration produces
127 protocol violations and 32 corrupt words in simulation.

The way past it is **a wider bus, not a faster clock.** Sixteen data lines at 25 MHz gives 100 MB/s
with the edge rate unchanged, which is far kinder to hand wiring than doubling the clock would be.
Eight more level-shifted wires buy more than twice the clock, and that is a generalisable lesson for
the rest of this project.

---

## The strobe cannot be used, which shapes the whole read path

Datasheet page 115, in substance: *with the DLL disabled, the value of tDQSCK could be larger than
tCK.* The strobe the chip returns can arrive more than a whole clock after expected, and the
datasheet declines to bound it.

So read data is **not latched on DQS.** It is sampled at a fixed offset from our own clock, and that
offset is the one number in the entire design that cannot be calculated — only found on the bench.

Two knobs cover it: which memory clock the first beat lands in, and where inside that clock to look.
Both are **settable at runtime**, which matters more than it sounds. As build-time parameters, every
wrong guess cost a synthesis, a place-and-route and a reflash. As runtime values, the Teensy sweeps
the whole space in about a second and prints a grid of which settings return data intact, choosing the
middle of the widest clean run so there is margin on both sides.

Simulation confirms the range is genuinely continuous: a device told to return data 10, 11, 12, 13 or
14 half-clocks late can be tuned in every time.

Writes are the mirror case. The chip must capture write data on the strobe *we* generate, so DQS is
driven out with a preamble and the data centred on its edges. DQS is a **differential pair** — drive
only the true half and the chip's write receiver, a comparator, has no reference and the data lands
at random.

---

## The design trick: impersonate a PSRAM

The Teensy already has a memory-mapped quad SPI peripheral with a spare chip select. If the FPGA
answers the command set a PSRAM answers, the DIMM appears as ordinary pointers, the AHB bus does the
fetching, and **the processor spends its cycles on arithmetic instead of moving bytes.** That is the
whole argument against bit-banged GPIO, which would be comparable on paper and would consume exactly
the cycles the real work needs.

The difficulty is that FlexSPI **cannot be stalled.** Once the dummy cycles end it clocks data out
relentlessly, and a DDR3 random read takes longer than the six dummy cycles a stock PSRAM sequence
leaves. Two things close it: a far longer dummy window, since the LUT field is eight bits and allows
up to 255 cycles, and keeping the link clock no faster than the memory can supply.

That second rule is what sets the configuration table, and it has an awkward corner. The slowest
FlexSPI2 can be made to run is **49.5 MHz**, its slowest source divided by eight, which consumes
24.75 MB/s. So a 6.25 MHz memory clock supplying 12.5 MB/s cannot be matched by slowing the link —
the transaction length is shortened instead, so the dummy window covers the whole fetch rather than
relying on the fill staying ahead of the reader.

| memory clock | DDR3 supplies | link | consumes | data lines | verified |
|---|---|---|---|---|---|
| 6.25 MHz | 12.5 MB/s | **one** pin at 49.5 MHz | 6.2 MB/s | 12.5 Mb/s | yes |
| 12.5 MHz | 25.0 MB/s | four pins at 49.5 MHz | 24.75 MB/s | 25 Mb/s | **fails** |
| 25 MHz | 50.0 MB/s | four pins at 79.2 MHz | 39.6 MB/s | 50 Mb/s | yes |

**The middle row is unreachable, and the reason is worth stating plainly because it looks workable.**
FlexSPI2's slowest possible clock is 49.5 MHz, its slowest source divided by eight. On four lines that
consumes 24.75 MB/s against a 12.5 MHz memory's 25 MB/s: a 1% margin. One percent is not a margin.
Every DRAM request carries fixed overhead no matter how the transfers are chunked, so the reader
catches up and returns bytes that have not arrived. Three of seven end-to-end cases fail at exactly
that pairing, and the link cannot be slowed to fix it.

The way out is a **narrower** link, not a slower one. One pin at the same 49.5 MHz consumes 6.2 MB/s,
leaving a factor of two in hand. That also halves the data line rate to 12.5 Mb/s, which is inside a
TXB0108's rating where 50 Mb/s is not — so the slow configuration is the one that can be built with
translators most people already own, and it is roughly a fifth the speed of the PSRAM it replaces.
Both rows marked verified pass the end-to-end test; the build script runs both.

---

## The rate rule, which turned out to be the hardest constraint in the design

Not the DRAM timings, not the strobe, not the level translation. **The peripheral that consumes the
data cannot be stalled and cannot be slowed below a floor**, and everything else has to be arranged
around that.

It bit three times. The testbench was first written pairing a 100 MHz link with a 25 MHz memory, both
worth 50 MB/s, which violated the rule the design was built around and made reads fail in a way that
looked like a buffer fault. The recommended first-light configuration was then set to 12.5 MHz and
never simulated; it fails. And the 25 MHz setting was briefly 99 MHz, which is 49.5 against 50 and
also fails. The working figure is 79.2 MHz, a 21% margin, and it is the fastest value actually
verified rather than merely believed safe.

The sketch now refuses to compile the configurations that cannot work, and carries an arithmetic guard
for the case where someone edits the numbers instead of the mode.

## The hardware finding that nothing in simulation could have caught

**VREF is mandatory and is the easiest thing in the world to omit.** DDR3 needs two reference
voltages at half of VDD, 0.750 V, on UDIMM contacts 1 and 67, each within ±1% DC with a separate
limit on AC noise. Every input comparator on every chip measures against them. Without them the
module does not respond at all and looks like a dead part.

Use **100 Ω divider pairs, not 1 kΩ.** The input leakage of sixteen chips sags a 1 kΩ divider by about
16 mV, which is 2% and out of spec on its own; at 100 Ω the same leakage costs 1.6 mV. One independent
divider per reference, each decoupled at the contact it feeds.

Two part findings that change what to order:

- **A TXB0108 cannot carry DDR3 data at 25 MHz.** Its datasheet gives 20 to 100 Mbps depending on
  rails and instructs that any pull resistor exceed 50 kΩ — which is how it tells you the output drive
  is deliberately weak. DDR3 data is double-rate, so 25 MHz means 50 Mbps. At 6.25 MHz it is 12.5 Mbps
  and comfortably inside, so a TXB0108 is a legitimate **first-light** part and not a full-speed one.
- **An SN74LS245 is not a level translator.** One supply, cannot output 1.5 V, 2.0 V input threshold
  so it cannot read 1.5 V either.

For the 26 output-only lines, plain resistor dividers are the **right** answer rather than a
compromise: stronger than a weak-drive translator, no direction logic to confuse, and incapable of
oscillating. 1.2 kΩ / 1.0 kΩ settles in 11 ns, fine below 12.5 MHz; 330 Ω / 270 Ω settles in 3 ns for
25 MHz at 5.5 mA per line.

Rail budget: about **0.5 A at 1.5 V** with the second rank held off by grounding its clock enable,
from standby figures of 32 to 55 mA per chip plus 12 mA of refresh. Size the supply for 1.5 A, and
bring 1.5 V up before or with 3.3 V, because the output dividers otherwise inject current into
unpowered inputs.

---

## Four gateware faults worth recording, because they generalise

Each of these was found by building a device model that **checks its master** rather than merely
responding to it, and by driving the whole chain with a bus master that reproduces what FlexSPI
actually emits. All four are arithmetic or structural, and all four would have been real on silicon.

**Read latency is the datasheet figure plus one.** The datasheet measures from the edge on which the
*device* latches a command. This controller registers commands on the falling edge deliberately, to
buy half a memory period of setup, so the device sees them one clock after they are issued
internally. Using the datasheet number directly samples one clock early and returns the previous
burst's last two bytes in front of every word — a very convincing wrong answer.

**A ready signal must mean "accepting now", not "available".** Requests are latched only on a memory
clock edge, which is one fabric cycle in four. An unqualified ready is true for three cycles on which
nothing can be accepted, and a requester that drops its valid on seeing ready loses the request three
times in four. The failure is silent and total, and the symptom is a bridge that waits forever for
data it never asked for.

**A flip-flop has one asynchronous reset.** Listing both chip select and reset as edge events is
legal Verilog and unsynthesisable. Registers that must *survive* chip select — a mode flag, a window
base, a calibration value, a byte count read after the fact — need their own blocks with their own
single reset.

**Cross clock domains with a toggle, not a pulse.** A one-cycle pulse handed between two unrelated
clocks can be missed or latched metastable. Here the symptom would have been a calibration sweep that
occasionally lies, sending someone hunting a hardware fault that does not exist.

A fifth is a Verilog trap rather than a design error: **`cell` is a reserved word** in Verilog-2001
configuration blocks, and a function named `cell` produces a syntax error on its own declaration line
with no hint why.

---

## What is deliberately not built

**Read-ahead.** A prefetch buffer fetching the next chunk while the current one is clocked out would
decouple link speed from memory speed entirely. It does not exist. An earlier draft of the source
comments described it as though it did, which is how a design acquires features everyone believes in
and nobody built; the comment now says so explicitly.

**ZQ calibration**, which trims output impedance and termination. Nothing is terminated at this speed.

**Bank interleaving, write levelling, and the second rank.**

---

## Where this leaves the machine

For the nine-Teensy array, the honest read is that this is a **capacity** tool, not a speed tool. It
trades 16 MB at 18.2 MB/s for 256 MB at 19.5 MB/s: a 16× capacity gain for a 1.07× throughput gain,
and only 0.29× until the level translators are upgraded.
Whether that is worth the wiring depends entirely on whether the models being run are capacity-bound
or throughput-bound, and by [33](33-a-30b-runs.md) the answer for mixture-of-experts models is
clearly capacity.

The broader lesson is the one in the first table. This project has treated memory as the scarce
resource throughout, and for a single Teensy above roughly 66 MB/s **it is not** — the processor's own
unpacking is. Any further effort on that node belongs in the kernel, not the memory system.

Reusable guide: [firmware/bench-one/fpga/ddr3_ice40/](../firmware/bench-one/fpga/ddr3_ice40/)
Bench wiring sheet: https://claude.ai/code/artifact/d39d40f3-1e30-4302-9495-690aafe3a355
