# 45 — The Teensy had been running the scalar kernel all along

Date: 2026-09-11. Status: **measured on hardware.** Teensy 4.1 at 600 MHz, `unpack_teensy`, kernel in
`firmware/bench-one/shared/gguf_dot.c`.

This project's headline arithmetic figure is 39.3 MB/s of Q4_K weights on a Teensy, and the case for
putting inference on FPGA fabric is built on it. `gguf_dot.c` has a hand-written AVX2 path and a
hand-written NEON path, and its header says the NEON one exists because "every weight-carrying node in
BENCH ONE is ARM or FPGA fabric".

**A Teensy 4.1 is a Cortex-M7.** ARMv7E-M, no NEON. The NEON path never applied to it, and the
benchmark never printed which path it had measured, so the figure the architecture rests on was the
portable scalar reference.

---

## The result

Both formats, both paths, one binary:

| | scalar | Cortex-M7 DSP | | bit-identical |
|---|---|---|---|---|
| Q4_K | 42.70 MB/s | **61.97 MB/s** | 1.45× | `0x430F608D` |
| Q6_K | 40.88 MB/s | **67.91 MB/s** | 1.66× | `0x4C525A9A` |
| **the real 69/31 mix** | 42.12 MB/s | **63.70 MB/s** | **1.51×** | |

Per weight rather than per packed byte, since the two formats pack differently: Q4_K goes from 75.9 to
110.2 million weights per second, Q6_K from 49.8 to 82.8. Q6_K's higher figure in megabytes is only
because it spends 210 bytes on the 256 weights Q4_K fits in 144.

The mix row is the one that matters. Time adds and rates do not, so the ceiling for a file that is 69%
Q4_K and 31% Q6_K by bytes is the harmonic mean, which makes the slower format count for more than its
share.

The host suite still passes, including `dot_verify`, which compares the AVX2 path against scalar on
two assertions. The new paths compile out entirely on anything that is not an ARMv7E-M part.

### Why both numbers had to come from one binary

The scalar Q6_K rate measured **33.00 MB/s in one build and 40.88 in the next**, with that kernel not
touched between them — a 24% swing from nothing but code moving in ITCM as other code was added around
it. A speedup claim of 1.66× compared against a baseline from a different build would be mostly noise.

So the sketch runs both paths in the same binary, back to back, switching between them with
`gguf_dot_force_scalar` at runtime. That was done to prove bit-identity and it turned out to be
necessary for the timing as well.

---

## Why the DSP extensions fit this kernel unusually well

`SXTB16` takes a word and spreads two of its four bytes into two sign-extended 16-bit lanes — bytes 0
and 2 plainly, bytes 1 and 3 with a rotate folded into the same instruction. `SMLAD` multiplies two
such lanes pairwise and adds both products to an accumulator, in one instruction.

What makes it fit is that **both operands take the same treatment.** A word of four packed nibbles and
a word of four int8 activations, each split by `SXTB16`, come out paired correctly with no shuffling
and no precomputed activation table: lanes {0,2} of the weights meet lanes {0,2} of the activations.
Four weights, four multiply-accumulates, two instructions.

The high nibbles need no second extraction path either. Shifting the whole word right by four moves
each byte's high nibble into its own low-nibble position; the bits that cross a byte boundary land in
the high nibble of the byte below and are removed by the same `0x0F0F0F0F` mask the low-nibble case
uses. One shift serves all four.

### Bit-identical by construction, not by luck

`dot` and `sum` are sums of int32 products. Integer addition is exact and associative, so reordering
them changes nothing. Everything outside the two inner loops is the reference's, copied unchanged —
including the double accumulator and the order of the per-sub-block float arithmetic.

The sketch proves it rather than asserting it: `gguf_dot_force_scalar(1)` switches the reference back
on at runtime, so one binary runs both over the same bytes and prints both results as raw bit
patterns.

---

## Where the time actually goes, measured rather than estimated

Document 42 measured a bare 4-bit kernel — constant zero point, integer output — at **2.07 cycles per
weight**. Q4_K runs at 5.57. I estimated the difference was mostly the per-sub-block double arithmetic
and put it at 61% of the time.

**That estimate was wrong, and measuring it took one instrumented entry point.**
`gguf_dot_q4k_stage` runs the same kernel with one stage removed at a time, so subtracting two timings
attributes cost to exactly one stage:

| what is running | MB/s | cycles per weight |
|---|---|---|
| everything | 60.64 | 5.57 |
| no float or double arithmetic | 75.59 | 4.47 |
| nor the 6-bit scale unpack | 92.81 | 3.64 |

| stage | cycles per weight | share |
|---|---|---|
| the nibble loop | 3.64 | **65%** |
| the 6-bit scale unpack | 0.83 | 15% |
| the float and double arithmetic | 1.10 | **20%** |

So the double arithmetic is a fifth of the cost, not three fifths, and moving it out of double — which
would change the result bits and need the float reference to adjudicate — is worth much less than it
looked. **The nibble loop is still where the time is**, at 3.64 cycles per weight against the bare
kernel's 2.07.

### And the reason for that gap is a sum that does not belong in the loop

Q4_K has a per-block minimum, so its inner loop computes the sum of the activations alongside the dot
product: two extra `SMLAD` for every four weights, which is most of the difference from 2.07.

**In a matrix-vector product that sum is the same for every row.** The activation vector is quantized
once and then every row of the weight matrix is dotted against it, so the per-32 activation sums could be
computed once per vector instead of once per row — and they are integers, so removing them from the loop
is bit-identical, not a numerics change.

It is now implemented and measured. `gguf_act_sums` computes the n/32 sums once, and
`gguf_dot_q4k_presum` takes them:

| Q4_K | MB/s | cycles per weight |
|---|---|---|
| DSP path, sums recomputed per row | 60.68 | 5.56 |
| **DSP path, sums hoisted out** | **66.97** | **5.04** |

**1.10×, bit-identical** — `0x430F608D` either way, because the sums are the same integers and the float
arithmetic downstream never sees a difference. The nibble loop goes from 3.63 to 3.11 cycles per weight,
which also includes moving it to eight bytes an iteration.

It does not reach the bare kernel's 2.07, and that residue is real: the bare kernel has one scale for
everything and folds its zero point into a closed form, where Q4_K has eight scales and eight minimums
per 256 weights and has to visit each sub-block separately. That structure is the format, not the code.

So the whole arithmetic result for the session, all of it bit-identical to the reference:

| | scalar | best | |
|---|---|---|---|
| Q4_K | 42.70 MB/s | 66.97 MB/s | 1.57× |
| Q6_K | 40.91 MB/s | 67.69 MB/s | 1.65× |
| **the real 69/31 mix** | 42.13 MB/s | **67.2 MB/s** | **1.60×** |

A note on the unrolling, since document 41 is about exactly that going wrong: this is compute-only code
with no timing specification attached to it, so a different instruction sequence changes the speed and
nothing else. The PSRAM case was different in kind — there the no-op count *was* the specification, and
changing the surrounding loop changed what the number meant.

**Q6_K got a path too, and gains more than Q4_K: 1.66×.** Its six-bit value is split across two arrays
and biased by −32, which needs one instruction Q4_K does not. The bias cannot be applied with an ordinary
subtract, because a byte lane below 32 would borrow into the lane above it; `SSUB8` subtracts all four
lanes independently and is a single instruction.

The bias also does *not* factor out the way Q4_K's zero point did, and that asymmetry is worth keeping in
mind. It could: the sum of (u−32)·x is the sum of u·x minus 32 times the sum of x. But that trades one
`SSUB8` per four weights for a running sum of activations, which costs two more `SMLAD` per four weights.
The identity pays when the correction is needed anyway — Q4_K's per-block minimum already demands a sum
of activations — and loses when it is not. Q6_K has no block minimum, so the bias stays in place.

---

## What this does and does not change

It does **not** change the conclusions of document 43. At hidden size 512 a layer is 152 ms of bus
against 60 ms of Q4_K arithmetic; making the arithmetic 1.45× faster takes that to 41 ms, so the layer
goes from 212 ms to 193 ms — 9% — and the bus still dominates by nearly four to one. The ordering of
priorities is unchanged: bus rate, then capacity, then nodes, then arithmetic.

It does change what the FPGA comparison should quote. A Cortex-M7 does **119.1 million Q4_K weights per
second**, not 75.9 million, and on the real 69/31 mix it is 1.60× the figure the project's record carries.
The gap to a fabric lane is that much narrower.

---

## The structural fault, which is the same one as document 41

The benchmark could not name its own code path. It measured whatever `dot_q4_k` dispatched to, printed
a number, and that number became an architectural argument — while a preprocessor condition written for
a different ARM core silently decided what was being timed.

`unpack_teensy` now prints `kernel path: Cortex-M7 DSP` as its first line of output, and runs both
paths and compares them before reporting any rate at all.

This is the second time in one day that a figure turned out to describe code other than the code that
shipped. Document 41's version was an unrolled loop; this one is a `#if`. The lesson is the same and
worth stating as a rule: **a benchmark that cannot name the implementation it measured is one rename
away from measuring the wrong thing, and will not tell you when it does.**
