# 49 — The Teensy does the arithmetic, the Luckfox does everything else

*Measured 2026-09-12 on the 8-chip perfboard build. Every figure here came off the hardware; the
firmware image and both ends of every exchange are in `bench-archive/20260912-031843-psram_worker/`.*

## What this changed

The Teensy used to decide things. It discovered its own banks, chose its own timing, ran its own soak,
kept its own counters and printed its own report. All of those are decisions, and a worker that holds
decisions holds state, and state is what makes a measurement drift between runs.

Now it holds two things: 64 MB of weights on a bus it drives itself, and a multiply-accumulate kernel.
It answers nine commands on a UART and returns numbers.

| | |
|---|---|
| `I` | identity: bank size, block size, bit widths, clock rate |
| `P <kind> <y>` | probe one select for an identity |
| `B <kind> <y>` | select a bank and put it in the current bus mode |
| `Q <mode>` | bus mode: quad on four lines, or single-bit |
| `T <wi> <ri> <burst>` | write index, read index, burst |
| `U <nops>` | chip-select setup before the first clock edge |
| `F <addr> <len>` | fill with the pattern |
| `M <addr> <len> <bits>` | read and multiply-accumulate, 4-bit or 2-bit |
| `V <addr> <len>` | verify the pattern back |

Nothing loops forever, nothing runs unasked, and every reply carries the cycles it took so the host
times the work rather than the round trip. The Luckfox owns discovery, the bus mode, the timing, the
burst, the benchmark and the log — in [`drive.py`](../firmware/bench-one/luckfox/drive.py).

The weights never cross the wire. A megabyte at 1 Mbaud is eight seconds; the same megabyte out of
PSRAM is a sixth of a second. So the host sends the **rule** that generates the weights, one command,
and the Teensy materialises them at bus speed. Link latency then shows up as a constant near 80 ms a
pass rather than as a bandwidth ceiling.

## The headline: two bits a weight is 1.96× four bits

| bit width | weights a byte | MMAC/s over 7 banks | MB/s |
|---|---|---|---|
| 4-bit | 2 | 6.56 | 3.28 |
| 2-bit | 4 | 12.84 | 3.21 |

The bus moves bytes, and a byte costs the same whatever is packed into it, so the only lever left after
the clock, the burst and the instruction count have all been exhausted is weights per byte. The 2-bit
kernel extracts four weights from each byte for the same 1.25 instructions a weight as the 4-bit one:
for a word `v`, lane `j` is `(v >> 2j) & 0x03030303`, `SXTB16` of that gives weights `{j, 8+j}` and the
rotated form gives `{4+j, 12+j}`. Four lanes, eight `SMLAD`s, sixteen weights.

**Both kernels are verified, not assumed.** The host computes the expected partial sum in Python on its
own ARM core and compares. Integer arithmetic, so a correct kernel is bit-identical and there is no
tolerance to argue about:

```
4-bit: teensy -90112       python -90112       MATCH
2-bit: teensy -16384       python -16384       MATCH
```

Across ten rounds of 7.3 MB a pass — 146 MB of weights read under sustained load — every partial sum
and every timing was identical to the digit.

What two bits costs in accuracy is not measured here and is not free: four levels against sixteen is a
perplexity question for the model, not a throughput question for this bench. This measures the ceiling
it buys.

## Six banks were returning garbage, and had been for some time

The previous headline for this build was 7.38 MB/s and 14.76 MMAC/s across all eight banks. That
number was throughput over bytes that were not the bytes that had been written. The old sketch said so
in its own log — `ANSWER CHANGED` against six banks, `0 of 8 banks hold their own data` — and then
reported the aggregate anyway.

### What was ruled out, by measurement

| cause | how it was tested | result |
|---|---|---|
| chip absent | 250 kHz identity probe, all ten selects | all six answer `0D 5D` |
| shared chip select | per-bank tag, fill all, read each against every tag | no off-diagonal zero: every bank holds only its own data |
| bus contention | same test | ruled out by the same evidence |
| the write direction | single-bit read of quad-written data at four addresses | correct bytes, correct tag: **the write lands** |
| one broken data wire | wrong bits counted per line over 32,768 nibbles | spread evenly over all four, and the ranking shuffles between runs |
| clock speed | 10.7 MHz down to 2.3 MHz | no improvement at any speed |
| burst length | 8, 12, 16, 24, 32, 48, 64 bytes | shorter is better, never clean |
| wait cycles after the address | 4, 5, 6, 7, 8, 10 | 6 is right and is the only value the good banks accept; nothing helps the others |
| sample instant in the clock phase | 7 positions, 0 to 48 no-ops | no effect: the window is wide |
| chip-select setup | 0 to 256 no-ops | transforms Y1, does nothing for the rest |
| bus keeper, hysteresis, drive strength, pull-up | four pad configurations | no effect |

The one thing that does change the answer: **every one of those chips reads correctly when it drives
one line instead of four.** Same clock, same ground, same supply, same command phase. That is the whole
difference.

### The fix that got 48 MB back

A chip in quad mode does not understand a single-bit command, so choosing the single-bit read means the
whole bank runs single-bit and is never given the `0x35` that enters quad mode. That cost one wrong
turn worth recording: a single-bit read issued to a chip still in quad mode returns garbage from every
bank, including the two that are perfect, which looks exactly like the new path not working.

With both directions single-bit, **seven of eight banks verify clean over a full megabyte.** Only Y4 is
dead in every mode.

| bank | mode | read MB/s |
|---|---|---|
| CS0, CS1 | quad | 9.70 |
| Y0, Y1, Y2, Y3, Y5 | single-bit | 2.59 |
| Y4 | — | does not read in any mode |

56 MB addressable instead of 16. One bit a clock is a quarter of the bits, and a quarter of the bits
over 48 MB is worth more than all of the bits over nothing, because capacity is what this machine is
for.

## The banks are not interchangeable, so do not spread a model evenly

A sequential pass is the **sum** of the per-bank times, so its length is set by the slow banks, not by
the average. Filling the fast banks first and stopping is worth a factor of three at small sizes:

| model size | banks used | seconds a pass | effective MB/s |
|---|---|---|---|
| 8 MB | 1 | 0.82 | 9.70 |
| 16 MB | 2 | 1.65 | 9.70 |
| 24 MB | 3 | 4.74 | 5.07 |
| 32 MB | 4 | 7.82 | 4.09 |
| 40 MB | 5 | 10.91 | 3.67 |
| 48 MB | 6 | 13.99 | 3.43 |
| 56 MB | 7 | 17.08 | 3.28 |

This is the same load-balance result this project already measured on the render fleet, arriving from a
different direction: equal shares waste the fast nodes.

At 16 MB in quad mode the rate is **19.04 MMAC/s at 4 bits and 35.41 MMAC/s at 2 bits** — also verified
bit-identical. So there are two operating points and the model size picks between them.

## Two bugs worth keeping on the record

**The burst was computed from the read and applied to the write.** At a slow write setting the write is
the slower direction per byte, so a burst comfortable for the read held chip select low past the
refresh window on the write. The data never landed, and then every read setting looked broken because
there was nothing correct to read. That alone cost the first run six banks. The burst is now bounded by
whichever direction is slower, from a timed measurement of each.

**A sweepable sample point put two instructions inside the nibble loop.** A compare and a branch in the
clock phase, even with the count at zero, is a different timing specification wearing the same no-op
count — exactly the fault `docs/41` exists to prevent. It showed immediately: the two good banks
qualified at read 12 over 32 kB and then returned a different partial sum on every pass over 1 MB. The
knob had already answered its question (the sample instant does not matter) so it came out, and the
payload loop went back to the sequence every other sketch in the tree runs. `verify-psram-bus.py`
reports the remaining difference — the chip-select setup, which sits outside the payload loop — as a
declared variant.

And the rule that keeps being relearned: **a sweep is not a qualification.** 32 kB of silence bounds the
error rate below roughly one in thirty thousand; the fault that bit this project ran at one in six
hundred thousand. Every chosen setting is now confirmed over the span the benchmark will actually read,
and stepped back until it is clean — read first, then write, with the burst recomputed after every step,
because slowing down lengthens chip-select-low.

## What is left for the soldering iron

Nothing in firmware recovers the quad read on the six external chips. The evidence points at the
return path: writes land, single-line reads are perfect, four-line reads fail with errors spread
evenly across all four lines and shuffling between runs. Worth checking, in this order:

1. **Decoupling at the external chips.** Four output drivers switching together is the only thing that
   distinguishes a failing quad read from a working single-bit one. A 100 nF capacitor across each
   chip's supply pins, as close to the pins as the build allows, is the first thing to try.
2. **The ground return to the external chips.** Same reasoning: four drivers share it.
3. **The 22 Ω resistors and their joints**, reflowed. The intermittency between runs fits a
   high-resistance joint, which passes a logic level from the Teensy's strong driver and not from the
   chip's weak one.
4. **Y4 separately.** It answers the identity probe and reads in no mode at all, which is a different
   fault from the other five.

Until then the build is a working 56 MB at two operating points, and the firmware picks the right mode
per bank without being told.
