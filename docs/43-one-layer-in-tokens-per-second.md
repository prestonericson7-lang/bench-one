# 43 — One decoder layer, measured, in tokens per second

Date: 2026-09-11. Status: **measured on hardware.** Teensy 4.1 at 600 MHz, 4-bit weights streamed
from the 8 MB bit-banged PSRAM bank at the corrected settings of document 41.

Documents 40 to 42 measured megabytes per second and multiply-accumulates per second. This converts
them into the only unit the project is actually judged on, with no estimation step: a decoder layer at
hidden size *d* does a countable amount of arithmetic against a countable number of weight bytes.

| | per token, per layer |
|---|---|
| attention: Q, K, V and the output projection | 4*d*² multiply-accumulates |
| feed-forward: up and gate at 4*d*, down from 4*d* | 12*d*² |
| total | **16*d*²**, which at four bits is **8*d*² bytes of weights** |

Attention over the cache scales with context rather than *d*², and at these sizes it disappears next
to the projections. Sketch: `firmware/bench-one/tests/psram_layer/`.

---

## The result

| hidden size | weights per layer | read | compute | both | tokens/s, 24 layers |
|---|---|---|---|---|---|
| 256 | 512 kB | 37.99 ms | 3.61 ms | 41.48 ms | **1.004** |
| 512 | 2 MB | 151.94 ms | 14.42 ms | 165.93 ms | **0.251** |
| 1024 | 8 MB | 607.77 ms | 57.70 ms | 663.73 ms | **0.063** |

At hidden size 1024 one layer is exactly 8,388,608 bytes — the whole usable bank, to the byte.

---

## Which cost model this bus obeys, settled

This is the measurement the document exists for. Elsewhere in the project the cost of reading weights
and the cost of unpacking them are combined as reciprocals, on the reasoning that a hardware
controller transfers in the background so the slower of the two sets the pace. On a hand-driven bus the
processor *is* the transfer, so nothing overlaps and the two should add.

Both predictions were tested against the measured combined time at all three sizes:

| | 256 | 512 | 1024 |
|---|---|---|---|
| measured ÷ sum of the two | 0.997 | 0.997 | 0.997 |
| measured ÷ the slower alone | 1.092 | 1.092 | 1.092 |

**They add. 0.997 of the sum, identically at every size.** The reciprocal model is wrong for this bus
and right for a controller, and that is now measured rather than argued.

---

## And the consequence, which reorders the priorities

The ratio to the slower half is 1.092, and that number is more useful than it looks. If the transfer
overlapped the arithmetic perfectly the layer would take the slower of the two, so **perfect overlap
buys 8.4%.** Not 2×, not an order of magnitude. Eight percent.

It is small because the read so thoroughly dominates: 152 ms against 14 ms at hidden size 512. You
cannot hide 14 ms behind 152 ms and gain much.

What the bus rate buys, by contrast:

| | tokens/s at *d* = 512, 24 layers |
|---|---|
| measured, 13.80 MB/s, no overlap | 0.251 |
| perfect overlap at the same rate | 0.272 |
| hardware controller at 33 MB/s, with overlap | **0.656** |

**Widening the bus is worth 2.4×; overlapping the transfer is worth 8%.** Both are listed in this
project as reasons to move work off the processor, and they are not remotely the same size. The
perfboard rebuild's 20.2 ns per nibble target is the valuable half of that story.

---

## The kernel is not the problem and cannot become one

Compute-only reached **290.78 MMAC/s, which is 2.07 cycles per multiply-accumulate** — slightly better
than the 233.8 measured in document 42, because that version carried per-row accumulation this one does
not, and it lands almost exactly on the instruction-count floor predicted there: eight weights need one
load, four `SXTB16` and four `SMLAD`, their activations need four loads, thirteen instructions for
sixteen multiply-accumulates, 2.06 cycles each.

So at hidden size 512 the arithmetic takes 14 ms and the transfer takes 152. **The kernel is already
about eleven times faster than the bus it is fed by.** Every further cycle saved in it is worth
nothing at all until the bus changes, and the 1.5× that SMLAD bought over plain C buys zero tokens per
second today.

That is worth stating plainly because the opposite was the natural assumption. This project's earlier
record has unpacking as the binding constraint on a general-purpose processor, and at these sizes,
behind this bus, it is not even close.

### The real format, now measured rather than scaled

This kernel has a constant zero point and integer output. A real model stores Q4_K: 256 weights in 144
bytes, a float scale and minimum per block, six-bit scales per 32-weight sub-block. An earlier version of
this document estimated that layer time by scaling; `psram_layer` now streams genuine Q4_K blocks from
the bank and calls the shared kernel, so these are measured.

| hidden size | weights per layer | read | compute | both | tokens/s, 24 layers |
|---|---|---|---|---|---|
| 256 | 576 kB | 42.72 ms | 8.77 ms | 51.48 ms | **0.809** |
| 512 | 2304 kB | 170.86 ms | 35.09 ms | 205.92 ms | **0.202** |

The estimate for hidden size 512 was 212 ms against 205.92 measured, so it was sound — but it is better
to have the measurement, and `measured ÷ sum = 1.000` confirms the costs add in the real format exactly as
they do in the bare one.

Two differences from the bare-kernel table worth naming. Q4_K needs **9*d*² bytes per layer rather than
8*d*²**, because the scales and minimums are weights' worth of bus traffic that carry no weights — 2304 kB
against 2048 kB at hidden size 512, a 12.5% tax paid on the scarcest resource in the machine. And the
arithmetic is 35.09 ms against 14, so it goes from eleven times faster than the bus to **about five
times** faster, using the Cortex-M7 DSP kernel of document 45.

Still bus-bound, still the same ordering of priorities. **The honest end-to-end figure for a real model on
one Teensy is 0.809 tokens per second at hidden size 256.**

---

## What it means for the machine

The weights of a layer must cross the bus once per token, and decode generates one token at a time, so
there is no reuse to hide the transfer behind. The token rate is therefore bytes per layer divided by
bus rate, almost exactly, and everything else is a rounding error.

One Teensy with 8 MB of hand-driven PSRAM runs a 24-layer model at hidden size 256 at **0.809 tokens per
second in real Q4_K**, or 1.004 with a bare 4-bit kernel. That is a real, measured, end-to-end figure for
a board costing a few tens of dollars, and it is the first time this project has had one.

It also says where the next order of magnitude is, and it is not in the arithmetic:

1. **Bus rate.** 13.8 → 33 MB/s from the controller alone, if the perfboard lands. 2.4×.
2. **More banks.** Currently capped at 8 MB of 48 by a shared chip select, which is a soldering
   problem, not a design one. Capacity, not rate, but it decides which models fit at all.
3. **More nodes.** The per-layer cost is independent, so layers split across boards multiply
   throughput directly. This is the cheapest axis and the reason the machine is a fleet.
4. **The arithmetic.** Eleven times faster than needed on a bare kernel, five times on real Q4_K after
   document 45. Nothing here until the bus moves.
