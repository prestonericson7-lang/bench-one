# 36 — The shed: does $300k hold a 2 to 4 trillion parameter model, passively cooled

*Arithmetic, 2026-09-10, in `tests/shed.py`. Nothing here is measured on this hardware because none
of it is bought. Every figure is labelled paid, published, market or estimate, and the four estimates
that dominate the answer are named at the end.*

---

## The claim, and the verdict

Scale the FPGA-and-DIMM core, scale the Teensys and Luckfoxes alongside it, about $300k in parts, a
shed, no fans, a 2 to 4 trillion parameter model.

**It checks out, and the budget is conservative by a factor of two to five.** A 4T mixture-of-experts
needs $60k to $139k of parts depending on one unknown. The full $300k buys between 5.6 and 10.7 TB of
resident weights, which is a 10 to 20 trillion parameter model.

The passive cooling is not close to being the constraint. The machine draws 1.5 kW against a shed that
can shed 10.7 kW with no fan anywhere.

## 1. Capacity, and the thing everybody gets backwards

A 4T model at 4.5 bits a weight is **2095.5 GB** of resident quantized weights.

| | count | cost | watts | capacity |
| --- | --- | --- | --- | --- |
| 64 GB DDR4-3200 ECC RDIMM, used pull | 33 | $2,970 | 148 W | 2112 GB |
| FPGA carrier, 4 DIMM slots | 9 | $22,500 | 225 W | addresses all of it |

**The DRAM is 12% of the core cost.** Two thousand gigabytes of model memory costs under three
thousand dollars. Everything else in the core is the controllers.

That inverts the usual intuition and it is the single most important number in this document. The
scarce thing is not memory. It is *addressable* memory with a processor next to it, and that is a
board design problem rather than a purchasing problem.

## 2. Bandwidth scales with capacity, which HBM does not do

Every DIMM is an independent channel, so adding a gigabyte adds bandwidth in the same step.

| | 2T model | 4T model |
| --- | --- | --- |
| DIMMs | 17 | 33 |
| peak | 217.6 GB/s | 422.4 GB/s |
| real, at the measured 0.71 fabric duty cycle | 154.5 GB/s | 299.9 GB/s |
| read per token, at 5.5% active | 57.6 GB | 115.3 GB |
| decode | 2.68 tok/s | 2.60 tok/s |

Decode stays at about 2.6 tokens a second whether the model is two trillion parameters or four,
because the numerator and the denominator grow together. On HBM they do not: capacity arrives in
fixed 80 or 192 GB lumps and you buy bandwidth you cannot use at batch one.

**Sparsity is the precondition, not an optimisation.** A 2T *dense* model reads all 1047 GB every
token, which at 154 GB/s is nearly seven seconds and no amount of memory changes it. The 5.5% active
fraction used here is DeepSeek-V3's published ratio, and it is the reason the shed is possible at all.

## 3. Passive cooling passes with an order of magnitude of margin

A 2 × 3 × 2.4 m shed, 20 K internal rise, 0.25 m² of vents high and low.

| path | capacity |
| --- | --- |
| skin, 30 m² at 7 W/m²K combined convection and radiation | 4,200 W |
| stack effect, 0.27 m³/s of air drawn by its own buoyancy | 6,486 W |
| total with no fan | 10,686 W |
| the 4T machine draws | 1,503 W |

Fourteen per cent loaded. The stack term is what does the work, and it is the reason a shed is a
sensible enclosure rather than a joke: a 2.4 m column of air at a 20 K rise moves itself.

**The real thermal problem is the other path.** A DIMM with no air moving across it cooks regardless
of what the shed can shed. Heat spreaders and vertical card orientation so the stack flow passes
between cards are genuine design items, and they are where the passive claim will actually be won or
lost.

## 4. The muscle layer, priced honestly

At 10% of the compute budget:

| | count | cost | watts | memory |
| --- | --- | --- | --- | --- |
| Luckfox Pico | 1,133 | $10,197 | 793 W | 14.7 GB |
| Teensy 4.1 | 278 | $8,896 | 139 W | 4.4 GB |
| Lyra Ultra | 79 | $6,320 | 198 W | 31.6 GB |
| total | 1,490 | $25,413 | 1,130 W | 50.8 GB |

Two things have to be said about this.

**As memory it is a terrible deal.** 50.8 GB is 2.4% of a 4T model, and the same $25,413 spent on
DIMMs buys 18 TB. If the goal were capacity, none of this money would go here.

**It draws 75% of the machine's total power** for 2.4% of the memory. That matters: the core is
374 W and the muscle is 1,130 W. Passive cooling survives it with room to spare, but if the thermal
budget ever binds, this is the first thing to cut.

What the money actually buys is 1,490 processors that cost nothing to idle, and there is already a
measured result saying what that is worth: thirty-three boards rendered one frame as thirty-three
bands that stitched byte-identically across two different instruction sets. Observability, control,
real-time work and the whole visual surface come free because the silicon doing them was never going
to carry weights.

## 5. The same resident capacity in HBM

For 2095.5 GB:

| card | cards needed | cost with node overhead | watts |
| --- | --- | --- | --- |
| MI300X 192 GB | 11 | $297,000 | 11,138 W |
| A100 80 GB | 27 | $546,750 | 14,580 W |
| H100 80 GB | 27 | $1,093,500 | 25,515 W |

Against the cheapest of those, the shed at a $2,500 carrier is **5.0× cheaper and 7.4× lower power**.

**And it loses on throughput by two orders of magnitude, which has to be said plainly.** A rack of
HBM serves hundreds of tokens a second to many users at once. The shed serves about two and a half,
to one user. It is not a rival for *serving* a model. It is a rival for *having* one.

## 6. What the whole $300k buys

The carrier price is the dominant unknown, so here is the answer as a function of it.

| carrier each | 4T core | 4T total parts | versus MI300X |
| --- | --- | --- | --- |
| $2,500 | $25,470 | $59,862 | 5.0× cheaper |
| $5,000 | $47,970 | $86,332 | 3.4× cheaper |
| $10,000 | $92,970 | $139,274 | 2.1× cheaper |
| $20,000 | $182,970 | $245,156 | 1.2× cheaper |
| $30,000 | $272,970 | $351,038 | over budget, and no longer cheaper |

And spending the full budget on this shape:

| carrier each | DIMMs | resident | bandwidth | model size at 4.5 bits |
| --- | --- | --- | --- | --- |
| $5,000 | 168 | 10,752 GB | 1,527 GB/s | 20.5 T |
| $10,000 | 88 | 5,632 GB | 800 GB/s | 10.8 T |

So $300k is not the budget for 2 to 4 trillion. It is the budget for ten to twenty, provided the
carrier lands under about $10,000 and the model is sparse.

## 7. The four numbers that decide all of it, none of them measured

1. **What a DIMM-capable carrier really costs.** $2,500 is a guess and it is 60 to 88% of the core.
   The whole answer above is a function of this one line. There is no off-the-shelf $2,500 board with
   four DDR4 slots and four hard controllers; the nearest proxy is an Alveo-class card with 64 GB at
   $2-4k used, and the real answer is a custom carrier around a Zynq UltraScale+.
2. **The real duty cycle of fabric unpacking against four live DDR4 channels.** The 0.71 used here
   came from synthesis against a single throttled feed. Four concurrent channels will not behave the
   same.
3. **What a 2T-class mixture-of-experts actually activates.** 5.5% is DeepSeek's published ratio. The
   measured expert cache hit rate on a 30B was 42% at the memory available, and that is the number
   that decides how much of the active slice has to cross a bus at all rather than already being
   where it is needed.
4. **DIMM power under this access pattern.** 4.5 W each is a datasheet figure, and thirty-three of
   them is the largest single load in the core.

## 8. Running it

```
cd firmware/bench-one/tests && python shed.py --model 4t-moe --budget 300000
```

```
cd firmware/bench-one/tests && python shed.py --model 2t-dense
```

The second one exists to show why dense is not a candidate.
