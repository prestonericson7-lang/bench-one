# 42 — Matrix arithmetic, measured across all three memory tiers

Date: 2026-09-11. Status: **measured on hardware.** Teensy 4.1 at 600 MHz, operands in the 8 MB
bit-banged PSRAM bank of document 40 at the corrected settings of document 41.

Document 40 showed the memory works. That is a storage result and says nothing about whether the
machine can compute. This runs the arithmetic the project actually needs, in each memory tier, and
makes the arithmetic verify the bus as a side effect.

Sketch: `firmware/bench-one/tests/psram_matrix/`.

---

## The numbers

| workload | PSRAM | OCRAM | DTCM |
|---|---|---|---|
| 4×4 vertex transform, float | 23.2 MFLOP/s | 663.6 | **671.0** |
| int8 matrix-vector | 12.4 MMAC/s | 107.4 | 120.0 |
| 4-bit matrix-vector, plain C | 23.3 MMAC/s | 140.9 | 150.0 |
| 4-bit matrix-vector, SMLAD | — | — | **233.8** |
| 64×64 float GEMM, textbook loop | — | — | 130.6 MFLOP/s |
| 64×64 float GEMM, 4×4 blocked | — | — | **498.9 MFLOP/s** |

Vertices per second, the figure a graphics pipeline is judged on: **827 thousand from PSRAM, 23.96
million from DTCM.**

---

## The checksums are the point

Every tier computes the same kernel over the same bytes, and every result is printed as a checksum
that has to match.

```
checksums  PSRAM 0xC4D8CA1E  OCRAM 0xC4D8CA1E  DTCM 0xC4D8CA1E
```

They match, in all three workloads, every run. That does two things. It proves the bit-banged bus
returned every byte correctly, which is a stronger statement than a byte comparison because it is
made by the arithmetic that consumes the data. And it stops the compiler deleting the work.

**The first version of this test reported the on-chip case at 0.000 ms and 275 million MFLOP/s.** The
loop's result was never read, so the loop was removed, and a timed empty loop divided into a constant
operation count produces a number that looks like a measurement. Any benchmark whose output is
discarded is measuring nothing, and it will not say so.

---

## Two of these numbers were my own fault, not the chip's

### The textbook GEMM was 3.8× slower than the same arithmetic reordered

The i-k-j loop managed 9.19 cycles per multiply-accumulate on a floating point unit that the vertex
transform was driving at about 1.6 cycles per operation. That gap is not the hardware. The inner
statement is

```c
for (j = 0; j < N; j++) c[j] += av * b[j];
```

which loads `c[j]`, multiplies, adds and stores `c[j]` back on every iteration — two memory touches
and a store-to-load dependency for one useful multiply.

Blocking holds a 4×4 patch of the output in sixteen registers for the whole length of k. Each step of
k then costs four loads from A, four from B and sixteen multiply-accumulates: **sixteen useful
operations per eight loads instead of one per two**, with nothing stored until the tile is finished.

| | cycles per MAC | MFLOP/s |
|---|---|---|
| textbook i-k-j | 9.19 | 130.6 |
| 4×4 register blocked | 2.41 | **498.9** |

Element-by-element comparison of the two result matrices: **0 differing.** Same arithmetic, same
order of accumulation along k, only the order of the independent output elements changed. A fast GEMM
that computes something else is worth nothing, so the test checks.

### The 4-bit kernel had 1.5× left in it, which revises a project claim

This is the measurement the project's architecture rests on: unpacking, not bandwidth, is what limits
a general purpose processor on four-bit weights, and the size of that limit is the argument for
putting the matrix work on an FPGA. A claim that load-bearing deserves the best version of the thing
it is claiming about.

Two changes, neither of which alters a single result:

**Four bytes at a time as packed halfwords.** One 32-bit load brings in eight weights. Masking with
`0x0F0F0F0F` isolates the four low nibbles and a shift the four high ones; `SXTB16` spreads two of
those bytes into two 16-bit lanes, so four `SXTB16` turn one word into eight weights in four
registers. `SMLAD` then does two multiply-accumulates per instruction. Eight weights cost four
instructions instead of eight multiplies and eight adds.

**The zero point factored out of the loop.** Weights are stored biased, so the plain kernel subtracts
eight from every nibble: two subtractions per byte, for a constant. But the sum of (w − 8)·a over a
block is the sum of w·a minus eight times the sum of a, and the activations are known in advance. The
subtraction leaves the inner loop entirely and becomes one correction at the end. Where the block is
a whole number of passes over the activation vector it has a closed form and costs nothing at all.

The price is that activations must be pre-packed to match the order `SXTB16` produces — `{byte0,
byte2}` then `{byte1, byte3}` rather than consecutive — which is done once at startup.

| | cycles per MAC | MMAC/s | checksum |
|---|---|---|---|
| plain C | 4.00 | 150.0 | 668567 |
| SMLAD | 2.63 | 228.2 | 668567 |
| SMLAD, bookkeeping removed | **2.57** | **233.8** | 668567 |

Bit-identical throughout.

**So the honest figure for a Teensy 4.1 on a bare 4-bit kernel is 234 MMAC/s, not 150.**

### Which is not the same thing as the project's 39.3 MB/s, and the difference is the point

That distinction matters enough to state before the number gets quoted anywhere. This kernel has a
**constant zero point of eight** and produces an integer. The 39.3 MB/s recorded in documents 24, 27 and
28 is `gguf_dot_q` over **real Q4_K blocks**: 256 weights in 144 bytes, a float scale and a float
minimum per block, six-bit scales per 32-weight sub-block, and float output.

Converting to a common unit, because megabytes per second of packed bytes and multiply-accumulates per
second are not comparable as written:

| | weights per second |
|---|---|
| Q4_K, fused dot, measured at 39.3 MB/s of packed bytes | 69.9 million |
| this bare 4-bit kernel, plain C | 150 million |
| this bare 4-bit kernel, SMLAD | **291 million** |

**The bare kernel is 4.2× faster than the real format, and the gap is the format's metadata, not the
nibble unpacking.** Per-sub-block scales, the block scale and minimum, and the conversion to float cost
more than four times what extracting the nibbles and multiplying them costs.

So this does not replace the 39.3 MB/s figure and nothing that quotes Q4_K should switch to 291. What it
does is relocate the problem: the project's record says unpacking binds a general-purpose processor, and
on the evidence here the unpacking is the cheap part. Applying `SXTB16` and `SMLAD` to Q4_K's actual
layout, scales and all, is an unmeasured and probably large win, and it is now the obvious next thing to
try on the arithmetic.

And 2.57 cycles is close to the floor for this instruction set. Eight weights need one load, four
`SXTB16` and four `SMLAD`, and their activations need four loads: thirteen instructions for sixteen
multiply-accumulates. There is no four-lane 8-bit multiply on a Cortex-M7, so `SMLAD`'s two lanes are
as wide as the arithmetic gets. An FPGA lane does one multiply-accumulate per clock with no unpacking
at all, which is the whole argument, restated at 234 rather than 150.

---

## What the memory tiers cost

The 4-bit kernel runs **6.6× faster from DTCM than from PSRAM, so 85% of the PSRAM run is the bus and
not the arithmetic.**

That ratio is worse than it will be, and the reason matters. On a bit-banged bus the processor *is*
the transfer: it toggles every edge itself, so a read and a multiply cannot overlap at all. They add
outright. Elsewhere in this project the cost model adds read and unpack as reciprocals, which is
correct for a hardware controller where a read is a load and the controller does the work.

So these PSRAM figures are a lower bound, not a verdict. If the perfboard rebuild gets under 20.2 ns
per nibble the hardware controller becomes usable, the transfer stops consuming instruction slots,
and the two costs start overlapping again.

The OCRAM and DTCM columns are close — 663.6 against 671.0 MFLOP/s on the transform, 140.9 against
150.0 on the 4-bit kernel — which says the bus matrix is not the constraint for streaming reads at
these rates. DTCM's advantage is about 5%, not the order of magnitude the PSRAM column shows.

---

## What to take from this

- A Teensy 4.1 does **499 MFLOP/s** of float GEMM and **24 million 4×4 vertex transforms per second**
  from on-chip memory. Graphics-shaped matrix arithmetic is not the bottleneck on this part.
- It does **234 MMAC/s** on 4-bit weights, 1.5× the figure previously recorded, and that is within
  about 25% of the instruction-set floor.
- Fed from the hand-driven PSRAM bank those fall to 23 MFLOP/s and 23 MMAC/s, because the processor
  cannot transfer and compute at the same time.
- Two of the three slow results in the first run of this test were mine: a discarded return value that
  deleted a loop, and a loop order that spent nine cycles per multiply. **Measure the good
  implementation before concluding anything about the hardware.**
