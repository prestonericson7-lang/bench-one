# 50 — What this machine actually runs

*No new measurements. Every number below comes from the per-bank rates already on the hardware, in
`bench-archive/20260912-073808-psram_worker/`. This is the translation from MMAC/s, which means nothing
on its own, into model size and tokens a second, which is what the machine is for.*

## The measured bus, per bank

| bank | mode | 4-bit MB/s | 2-bit MB/s | 1-bit MB/s |
|---|---|---|---|---|
| CS0 | quad | 10.38 | 9.65 | 8.58 |
| CS1 | quad | 9.30 | 8.71 | 7.83 |
| Y0–Y5 (six) | single-bit | 3.46 | 3.38 | 3.24–3.40 |

A pass is **sequential** — one CPU, one bus — so its length is the *sum* of the per-bank times, not the
average. That single fact drives everything below.

## The rule that sets token rate

At batch 1 a transformer reads **every weight once per token**. So:

    seconds per token  =  model bytes / effective MB per second
    tokens per second  =  1 / that

There is no way around it at batch 1. The weights have to come off the bus and there are no shortcuts.

## What fits, and how fast it runs

At one bit a weight, 64 MB holds **512 million parameters**. That is the headline capacity.

| params | bits | bytes | where it lives | seconds/token | tokens/s |
|---|---|---|---|---|---|
| 64 M | 1 | 8 MB | CS0 alone | 0.93 | **1.07** |
| 128 M | 1 | 16 MB | CS0 + CS1 | 1.95 | **0.51** |
| 256 M | 1 | 32 MB | both quad + 2 slow | 6.80 | 0.15 |
| 512 M | 1 | 64 MB | all eight | 16.50 | 0.061 |
| 32 M | 4 | 16 MB | CS0 + CS1 | 1.63 | **0.61** |
| 128 M | 4 | 64 MB | all eight | 15.50 | 0.065 |

Real models, at one bit:

| model | params | 1-bit size | fits? | tokens/s |
|---|---|---|---|---|
| GPT-2 small | 124 M | 15.5 MB | yes, both fast banks | 0.53 |
| SmolLM-135M | 135 M | 16.9 MB | yes, spills one slow bank | 0.35 |
| SmolLM-360M | 362 M | 45 MB | yes, six banks | 0.10 |
| Qwen2.5-0.5B | 494 M | 62 MB | yes, only just | 0.063 |
| TinyLlama | 1.1 B | 137 MB | **no** | — |
| Llama-3.2-1B | 1.24 B | 155 MB | **no** | — |

So the honest summary: **this build runs a GPT-2-class model at about half a token a second, or a 64
million parameter model at about one token a second.** A half-billion parameter model fits in the
memory and takes sixteen seconds a token.

## Where the time actually goes

Of the 16.5 seconds in a full 64 MB pass, **14.5 of them are the six external banks.** They hold 75% of
the memory and consume 88% of the time, because they read one bit to the clock while the two onboard
chips read four.

| | share of memory | share of time |
|---|---|---|
| CS0 + CS1 (quad) | 25% | 12% |
| Y0–Y5 (single-bit) | 75% | 88% |

## The three levers, in order of size

**1. Get the six external banks reading in quad — 2.6x.** They already take quad *writes* perfectly, and
in the last dump Y1 and Y3 returned 24 bytes of quad read with zero errors. The failure is not constant:
a bank reads correctly for part of a burst, the chip stops driving, and it recovers cleanly at the next
burst boundary. If all eight banks ran quad, a full pass drops from 16.5 s to 7.5 s and the 512M model
goes from 0.061 to **0.134 tokens/s**. This is the single biggest number on the table and it is still
open.

**2. Batching — linear in the batch size, and it costs nothing but code.** One weight pass currently
produces one token. It could produce as many as there are concurrent requests, because the weights read
are the same for all of them. At batch 8 the 512M model goes from 0.061 to **0.49 tokens/s** aggregate,
and at batch 32 to 1.95, with no hardware change at all. The machine is bandwidth-bound and batching is
the standard answer to bandwidth-bound.

**3. Quantisation — already taken.** Four bits to one is 3.69x measured and verified. There is nothing
below one bit.

## What this says about the design

The thesis for this machine is capacity from cheap parts, and on capacity it delivers: 64 MB of weights
on one $30 microcontroller, holding half a billion parameters, for the price of the PSRAM.

The cost is bandwidth. A bit-banged bus gives about 10 MB/s a chip at best and 3.4 at worst, against
roughly 900 MB/s for the DDR2 sitting on the Luckfox next to it. That ratio is the whole argument for
the FPGAs in the parts list: they exist to be the matrix engines precisely because a microcontroller
cannot move weights fast enough, and these numbers are the measurement that says by how much.

Nothing here is a reason to stop. It is the baseline the rest of the machine gets compared against, and
it is the first time this project has had one in tokens rather than in megabytes.
