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

| | MB/s of packed Q4_K | weights per second |
|---|---|---|
| scalar reference, measured today | 42.94 | 76.3 million |
| **Cortex-M7 DSP path** | **62.24** | **110.6 million** |

**1.45×, and bit-identical.** One Q4_K block through both paths returns `0x430F608D` either way.

The host suite still passes, including `dot_verify`, which compares the AVX2 path against scalar on
two assertions. The new path compiles out entirely on anything that is not an ARMv7E-M part.

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

## The bottleneck has moved, and that is the more useful finding

Document 42 measured a bare 4-bit kernel — constant zero point, integer output — at **2.07 cycles per
weight**. Q4_K now runs at 62.24 MB/s, which is 5.4 cycles per weight.

So roughly **3.3 cycles per weight, about 61% of the time, is no longer the nibbles.** It is
`gguf_q4k_scale_min` unpacking eight 6-bit scales and eight 6-bit minimums out of twelve asymmetrically
packed bytes, and then three double multiplies, a double subtract and a double add per 32 weights.

That figure is inferred from two measurements rather than measured directly, and it should be read as
an estimate. The direction is not in doubt, though: the comment above the double accumulator says one
double add per 32 weights is "free against the integer multiply-accumulate underneath it", and that was
true when the integer part cost 8.6 cycles a weight. It is not true now.

**The double accumulator should not simply be removed.** It is there because a float running total over
344 sub-block contributions lost 3.5 × 10⁻⁴ of relative accuracy on `ffn_down` and read as a kernel
bug. The precision problem was cancellation in the *sum*, not error in the terms — so computing each
sub-block term in float and accumulating into a double would likely keep the accuracy and remove most
of the cost. It would also change the result bits, so it is a numerics change that needs the float
reference in `tests/fast_path.c` to adjudicate it, not a free win. It is the next thing to try.

**Q6_K has no DSP path yet** and sits at 33.00 MB/s. The real model is 69% Q4_K and 31% Q6_K by bytes,
and time adds while rates do not, so the mix ceiling is the harmonic mean — which means Q6_K is worth
roughly a third of the remaining gain and is not optional.

---

## What this does and does not change

It does **not** change the conclusions of document 43. At hidden size 512 a layer is 152 ms of bus
against 60 ms of Q4_K arithmetic; making the arithmetic 1.45× faster takes that to 41 ms, so the layer
goes from 212 ms to 193 ms — 9% — and the bus still dominates by nearly four to one. The ordering of
priorities is unchanged: bus rate, then capacity, then nodes, then arithmetic.

It does change what the FPGA comparison should quote. A Cortex-M7 does **110.6 million Q4_K weights per
second**, not 76.3 million, and the gap to a fabric lane is that much narrower than the project's record
says.

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
