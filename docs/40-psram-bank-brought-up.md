# 40 — Six PSRAM on one Teensy: brought up, and what it cost

> **Corrected by documents 41 and 44.** The write 5 / read 8 settings below were the fast edge of
> their clean window and stopped passing the same day; the qualified configuration is write 6 /
> read 10 at **21.31 MB/s write and 13.80 read**, verified over eleven consecutive 8 MB rounds. The
> chip-select-low times in the burst table were inferred from payload throughput and are about 7%
> low; measured directly, 96 bytes is 6.93 us and 112 is already past the 8 us limit. And the 8 MB
> cap does not need the onboard chip desoldered -- one wire moves the decoder enable to pin 5 and
> the usable capacity becomes 48 MB.

Date: 2026-09-11. Status: **measured on hardware.** Every number here came off a Teensy 4.1 with six
ESP-PSRAM64H chips attached, one on the board's own RAM footprint and five on a breadboard behind a
74LVC138A.

This is the first document in the project whose figures are neither simulated nor traced to a
datasheet. It is also the first time the machine ran and did something.

---

## The result

| | |
|---|---|
| verified | 8,388,608 bytes, **zero wrong** |
| write | **23.69 MB/s** |
| read | **14.83 MB/s** |
| retention | holds after five seconds idle |

Driven entirely by GPIO. The hardware QSPI controller is not involved and, on this wiring, cannot
be — see the ceiling below.

For scale, the same chip through the controller on an unmodified Teensy measures 33.9 MB/s raw. A
hand-driven bus on a breadboard reaches 44% of that on reads and 70% on writes.

---

## The ceiling, and why the controller can never be used here

The bus needs **37.2 ns per nibble** to be reliable. Faster than that and reads corrupt.

FlexSPI2 cannot be clocked below **49.5 MHz**, which is 20.2 ns per nibble. That is its slowest
possible divider from its slowest possible source, verified against the clock table.

So the controller is **1.85× too fast for this wiring and has no setting that would work.** That is
why a sweep of all six achievable clocks came back silent, and why a sweep of all 32 read-sampling
positions at every one of those clocks came back silent as well. There was never a combination to
find.

**This gives the perfboard rebuild a target rather than a hope.** Get under 20.2 ns per nibble and
the controller becomes usable, which brings memory mapping and roughly 33 MB/s instead of 15. The
perfboard has to be about twice as good as the breadboard, and that is mostly about giving the four
data lines a ground to return along.

---

## Three settings, each measured rather than chosen

**Writes and reads want different timing.** Holding writes fixed and sweeping reads, reads break
below 8 no-ops. Holding reads fixed and sweeping writes, writes stayed clean to 5, the bottom of the
sweep. The write path only drives; the read path also has to get a value back out of `GPIO9_PSR`
between the edges, and that costs bus latency the write never pays.

One shared number throttles writes to the read's limit and gives away a quarter of the write rate.

| | no-ops | MB/s |
|---|---|---|
| write | 5 | 21.85 |
| read | 8 | 13.43 |

**Bursts pay until refresh stops them.** Every burst re-sends a command and a 24-bit address, and a
read adds six dummy clocks: fourteen nibbles of overhead whatever the payload.

| burst | read MB/s | chip select low |
|---|---|---|
| 16 B | 11.70 | 1.37 µs |
| 32 B | 13.43 | 2.38 µs |
| 64 B | 14.50 | 4.41 µs |
| 96 B | 14.87 | 6.46 µs |
| 128 B | 15.09 | **8.48 µs — over the limit** |

These chips are DRAM inside and only refresh while chip select is high, for at most 8 µs. 128 bytes
is measurably the fastest and is not usable: it would read correctly today and lose data that had
been sitting a while. 96 is the last size with real margin, and the retention test above exists to
prove that margin is real rather than assumed.

---

## Three bugs, all mine, all indistinguishable from hardware faults

The bus was declared broken three times before it was declared mine. Each of these failed at *every*
speed, in *every* width, in *both* directions, which is exactly what a bus too fast for its wiring
looks like.

**The clock never went back down.** Every edge ended high, so the next phase opened by writing the
clock high again, which is not an edge. The chip's reply arrived shifted by exactly one bit:
`0D 5D 53 31` came back as `06 AE A9 98`, which is that stream shifted right once and nothing else.

This one was blamed in turn on breadboard ringing, on a missing bulk capacitor, and on chip-select
contention. All three were plausible. All three were wrong.

**Two data lines could never be driven low.** The base register value kept D2 and D3 set from the
idle state while the nibble was only ORed in, and OR cannot clear a bit.

**Every nibble reset the bank.** The decoder's three address pins live in the same GPIO register as
the data lines, and the driver wrote that register whole. The idle value had been captured before the
first bank was selected, so every nibble quietly restored bank 0 and put a second chip on the bus.

### What actually found it

Not more theorising. Two implementations of the *same* identity read, placed side by side in one
sketch — the `digitalWriteFast` one already known to work, and the register one under suspicion —
and then **looking at the bytes rather than at a pass/fail count**. The one-bit shift was visible
immediately. It took one run.

The lesson is cheap and worth keeping: when a rewrite of a working thing fails in every
configuration, the rewrite is wrong, and the fastest route is to run both against a case whose
answer is known in advance.

---

## The chip select is shared, and that is a soldering problem

The second QSPI footprint's chip-select pad tore off during assembly. That pad is the only place
Teensy pin 51 exists on the whole board — on the pinout card every other QSPI signal is marked as
appearing twice, and 48 and 51 are not. The decoder's enable was therefore wired to the *first*
footprint's chip select, which the onboard PSRAM is already using.

Both now answer at the same instant, and the collision is proven rather than argued: writing a
different pattern to each of the five banks in order and reading them back gives

| bank | wrong |
|---|---|
| 0 | 256 |
| 1 | 256 |
| 2 | 256 |
| 3 | 256 |
| 4 | **0** |

Bank 4 was written last, so the onboard chip holds bank 4's pattern and agrees only with that one.

So **8 MB of the 48 is usable** until the first chip select belongs to one device. Either remove the
onboard chip, or lift only its pin 1 and run that leg to decoder pin 10, which turns it into a sixth
bank rather than a rival.

---

## Things ruled out, with the measurement that ruled them out

**Not the soldering.** All six chips return a correct `0D 5D` signature to a bit-banged identity
command, and all four data lines carry a block intact.

**Not the supply.** At an edge rate that fails, changing burst length from 4 to 32 bytes and adding
idle gaps from 0 to 50 µs moved the error count only between 666 and 691 out of 1024. Sag would swing
hard with both. It barely notices, which makes it signal timing rather than charge. The missing bulk
capacitor is worth fitting for other reasons and is not what limits the speed.

**Not the sampling point.** Six clocks × 32 delay taps, all silent. There is no moment in a 20 ns
cycle when the line is quiet, so no latch position can help.

---

## What is next

The perfboard rebuild, with the target above. If it lands under 20.2 ns per nibble the hardware
controller takes over and this driver becomes a fallback rather than the main path.

Until then the working configuration is in
`firmware/bench-one/tests/psram_driver/`: write at 5 no-ops, read at 8, 96-byte bursts, decoder parked
on the unconnected Y7.
