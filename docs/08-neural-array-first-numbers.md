# 08 — The neural array: first numbers

My own arithmetic, done before the research fleet reports, so there is something to check its
answers against. Every figure here shows its working.

**Headline: this is not slow. A small deep belief network trains on one Teensy in minutes.**

---

## The number that decides the architecture

For a fully-connected layer, **each weight is used exactly once per sample.** There is no data
reuse to amortise the fetch, so the machine is memory-bound unless the weights sit in fast memory.
That makes "where do the weights live" the whole design.

### What each memory can feed

PJRC's own wording for RAM1: *"tightly coupled memory... fast single cycle access to memory using
a pair of 64 bit wide buses."*

| Where weights live | Bandwidth | int8 MACs/s it can feed |
|---|---|---|
| **RAM1 / DTCM** | 8 bytes/cycle × 600 MHz = **4.8 GB/s** | 4.8 G |
| RAM2 / OCRAM | slower, DMA-optimised | lower, unmeasured here |
| QSPI PSRAM | ~88 MHz × 4-bit ≈ 44 MB/s theoretical, ~30 MB/s real | **~30 M** |

### What the core can consume

The Cortex-M7 DSP extension's `SMLAD` does **two signed 16×16 multiply-accumulates in one
instruction**. At 600 MHz that is 1.2 GMAC/s for int16. CMSIS-NN packs int8 four to a word, so
int8 lands somewhere between 1.2 and 2.4 GMAC/s in practice.

### Therefore

| Weights in | Supply | Demand | Verdict |
|---|---|---|---|
| DTCM | 4.8 GB/s | 1.2–2.4 GB/s | **compute-bound — the core is the limit** |
| PSRAM | 30 MB/s | 1.2–2.4 GB/s | **memory-bound by 40–80×** |

**Weights in DTCM or the machine is forty times slower.** That is the single most important
sentence in this document, and everything below follows from it.

---

## What fits

RAM1 is 512 KB per Teensy, shared between ITCM (code) and DTCM (data) through the configurable
FlexRAM. Call it **~400 KB of DTCM available for weights** after code.

Four Teensys is roughly **1.6 MB of single-cycle weight storage.**

The classic Hinton MNIST deep belief net, 784-500-500-2000:

```
784 × 500  =   392,000
500 × 500  =   250,000
500 × 2000 = 1,000,000
             ---------
             1,642,000 weights  =  1.64 MB at int8
```

**1.64 MB needed against ~1.6 MB available.** It essentially exactly fits across four Teensys —
but only if the 500×2000 layer is split, since 1 MB alone exceeds any single Teensy's DTCM.

That coincidence is the architecture: **shard the network so every weight lands in tightly coupled
memory**, and accept splitting the widest layer across two or three nodes.

---

## The first milestone, and it is small on purpose

Downsample MNIST to 14×14 and the network fits **entirely inside one Teensy**:

```
196 × 256  =  50,176
256 × 256  =  65,536
256 × 512  = 131,072
             -------
             246,784 weights  =  247 KB at int8   →  fits in one DTCM
```

### Time per epoch, with the working

CD-1 on the first RBM, 196×256 = 50,176 weights:

```
positive phase   v→h    50,176 MACs
negative phase   h→v    50,176
                 v→h    50,176
weight update           50,176
                        -------
                       ~200,000 MACs per sample

60,000 samples × 200,000 MACs      = 1.2 × 10^10 MACs per epoch
at 1.2 GMAC/s                      = 10 seconds per epoch
100 epochs                         = ~17 minutes
```

Even at a pessimistic 300 MMAC/s once stalls and overhead are counted, that is **40 seconds per
epoch and about an hour for a hundred epochs.**

You said it could think very slowly. It does not have to.

---

## The link, and why layer-wise training is the right choice

At 921600 baud each link moves 92,160 bytes/s.

| What crosses | Size | Time |
|---|---|---|
| All weights of the full DBN | 1.64 MB | **18 seconds** |
| One sample's activations, int8 | 500–2000 B | **5–22 ms** |

So **data parallelism is dead on UART** — synchronising gradients would cost 18 seconds per step.

But a deep belief network is trained **greedily, layer by layer**, and each RBM trains
*independently*. Teensy 1 trains layer 1 to completion, ships the learned features onward, and
Teensy 2 begins. **The link carries features, never gradients.**

That is not a workaround. It is the property that makes DBNs the right algorithm for this
hardware, and it is why the 2006 algorithm fits a 2026 constraint better than anything modern
would.

Ethernet, at 94 Mbit/s Teensy-to-Teensy, becomes worth adding only for the fine-tuning phase,
where the whole unrolled network is trained at once and gradients genuinely must cross.

---

## The dataset problem, which is real

MNIST is 60,000 × 784 bytes = **47 MB**. It fits nowhere on a Teensy. Streaming one epoch from
the Luckfox over UART is:

```
47 MB / 92 KB/s = 512 seconds
```

**Eight and a half minutes of pure data movement per epoch, against ten seconds of compute.** The
link would be the entire cost.

Three ways out, in order of how much I like them:

1. **Downsample to 14×14.** 60,000 × 196 = 11.8 MB. Fits in a Teensy's 8 MB flash if reduced
   further, or streams in 128 s.
2. **Keep the dataset in Teensy flash.** 8 MB of read-only weights and data, already on the board.
3. **Generate the dataset from the stack's own sensors.** This is the interesting one, and it
   removes the bottleneck entirely — the data never crosses a link because it is captured locally.

---

## Where the fabric earns its place

Not in the arithmetic. It will never beat 1.2 GMAC/s.

**In the input layer.** A 74HC165 chain latches every input at one instant on a single `/PL` edge —
19 chips is 152 bits captured simultaneously, and the 4051 tree adds 64 analog channels.

A neural network whose input vector is captured **at one instant across 152+ channels**, rather
than scanned one at a time, is a genuinely different sensor front end. Scan skew smears
correlations between channels, and correlation between channels is exactly what the network is
trying to learn.

That is the simultaneity advantage, applied to machine learning, and it is the part nobody else
has.

---

## What I am least sure of

- **The int8 MAC rate.** 1.2–2.4 GMAC/s comes from `SMLAD` being two 16×16 MACs per cycle. The
  int8 packing path through `SXTB16` may cost more cycles than I have credited. The research pass
  is instructed to recompute this independently.
- **Usable DTCM after code.** I assumed ~400 KB of 512 KB. The FlexRAM split is configurable and I
  have not verified the practical figure.
- **PSRAM bandwidth.** ~30 MB/s is from memory, not measured.

If the first number is wrong, epoch times move proportionally. If the second is wrong, the
sharding changes. Neither breaks the architecture.
