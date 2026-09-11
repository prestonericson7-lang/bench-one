# 25 — A real model across four nodes, and the four things that were wrong



`pipe_model` runs Qwen2.5-Coder 3B across four separate processes, each holding a slice of its
thirty-six layers and nothing else. No node holds the model. The ring produces correct text anyway:

```
Write a Python function that takes a list of integers ... ```python
```

Getting from "it works" to "it works well" took four fixes. Every one was found by a measurement, and
three of them were worth more than any hardware change available.

---

## Where it ended up

Four processes, eight cores between them, two OpenMP threads each:

| | rate |
|---|---|
| **prefill**, 180 prompt positions | **6.94 tok/s** |
| **decode**, 4 streams | **4.99 tok/s** |
| decode, per stream | 1.25 tok/s |
| stage utilisation | 73–86% |

One process on the same machine with all eight threads gets 10.5 prefill and 4.3 decode. So
**distributed decode now beats it** and distributed prefill does not, which is exactly what the two
bottlenecks predict: decode is memory-bound and gains from the pipeline overlapping, prefill is
compute-bound and splitting the same eight cores four ways adds hops without adding arithmetic. On four
genuinely separate machines prefill would scale; here it cannot.

### The progression

| change | decode | prefill |
|---|---|---|
| first working version, one token in flight | 1.55 | 1.59 |
| four sequences in flight | 3.63 | — |
| cost-balanced split | 3.39 | — |
| no round barrier | 5.25 | — |
| batched prefill, chunks sent one at a time | — | 1.59 |
| prefill chunks pipelined too | 4.99 | **6.94** |

3.2× on decode and 4.4× on prefill, from four code changes and no new parts.

---

## Cost of distributing, measured

| | |
|---|---|
| sum of every node's compute | 645.1 ms |
| total wall clock per token | 645.4 ms |
| **what the network cost** | **0.3 ms, 0.05%** |

One activation per hop, 8192 bytes, against 44.5 MB of weights per layer. Five thousand to one. That
ratio is why pipeline parallelism runs on a cheap link and why tensor parallelism never could.

---

## Fix 1 — one token in flight wastes n−1 nodes

The first working version produced 1.55 tok/s where one process produced 4.25. Not overhead: the
arithmetic is identical and the wire is 0.05%. With a single token in flight **only one node is ever
working**, because token *t+1* cannot begin until token *t* exists.

| | throughput |
|---|---|
| k = 1 | 1 / (sum of all stages) |
| k ≥ n | 1 / (slowest stage) |

**The machine is a multi-request server, not a single-chat box.** That is a design conclusion the
measurement forced. Cost: one KV cache per stream per node, and a stream id on the wire, which took the
header from 32 to 40 bytes and the protocol to version 2.

**The streams are also the correctness test.** Every stream gets the same prompt, so every stream must
produce the same text. Wrong cache indexing blends two conversations, which reads as the model losing
the thread rather than as a crash. All k outputs identical is the proof.

---

## Fix 2 — an even split is the wrong split

Four streams on an even split gave 3.63 tok/s, against a prediction of 1 / 275 ms = 3.64 from the stage
times. The pipeline was already perfect; the **cut** was wrong. The head did nine layers at 133 ms *and*
the output projection at 142 ms while every other node did nine layers and nothing else.

Layers are now assigned by **cost**, with the head charged for the projection first. Cost is not bytes
either: Q4_K unpacks at 15.69 GB/s here and Q6_K at 6.11, so a Q6_K tensor costs 2.6× the same bytes of
Q4_K. The projection on this model is Q6_K, which is why it hurt far more than its size suggested.

| node | layers | per activation |
|---|---|---|
| head | **1**, plus the projection | 18.3 + 142.8 ms |
| stage 1 | 11 | 168.2 ms |
| stage 2 | 14 | 176.1 ms |
| stage 3 | 10 | 149.5 ms |

The head holds one layer of thirty-six. Every node computes this from the same file and reaches the same
answer, so there is no negotiation and no coordinator.

---

## Fix 3 — the round barrier drained the pipe

Balanced and four-streamed, still 3.39 tok/s with stages 51–60% busy. Balanced stages that are half idle
are waiting on something, and it was the loop shape. "Send all k, receive all k, repeat" drains the
pipeline at every round boundary.

Priming with k and refilling on each arrival keeps exactly k in flight with no node ever waiting on a
sibling: **3.39 → 5.25 tok/s, utilisation 51–60% → 82–96%.** A 55% gain from deleting a barrier.

The same barrier existed in prefill, where chunks from different streams are independent even though
chunks within a stream are not. Pipelining those too: **1.59 → 6.94 tok/s.**

---

## Fix 4 — the bug worth remembering

Batched prefill produced correct text for the first token and nonsense after it:

```
The capital of France is Paris to capital capital capital capital
```

The batch path was right. The single-activation path was not. The stage had been changed to receive into
a batch buffer, but `model_layers` operates on `m->x`, so every decode step ran on stale memory.

**Output that starts correct and then degenerates is the signature of reading the wrong buffer**, not of
a broken kernel. The diagnosis came from adding a `--batch` flag and setting it to 1: that made prefill
use the same broken path, so the failure moved to the first token and pointed straight at it. A knob that
lets a suspect code path be turned off is worth more than an hour of reading.

---

## Fix 5 — the output head had to be split, and it is free

The 3B model does **not fit** two 1 GB boards with the output projection whole. 243.4 MB of head plus
15 layers on one board and 20 on the other leaves one layer of thirty-six with nowhere to live. That is
not a tuning problem, it is the model not fitting the hardware.

The projection is the one weight matrix that splits freely. Every logit needs the whole activation but
only its own row, so node A scores vocabulary 0 to 37,983 and node B scores 37,984 to 75,967 with no
communication at all. Each returns its best candidate, eight bytes, instead of 594 KB of logits.

A layer cannot do that: splitting one means exchanging activations inside attention several times per
token, and the exchange costs more than the weights saved.

Two consequences, and one of them looked bad at first:

**The input embedding now comes off the disk.** The tied tensor is read two ways — as a lookup it needs
one 4 KB row per token, and as the head it is a full matmul over 151,936 rows. Splitting it for the
matmul means no node holds the whole table, so the lookup reads one row from the file. That costs a
tenth of a millisecond and saves 243 MB of RAM, which is five layers.

**Each token now goes round the ring twice**, once for the layers and once for the projection. That
looked like a straight 17% loss:

| requests in flight | decode, split | decode, unsplit |
|---|---|---|
| 4 | 4.13 tok/s | 4.99 tok/s |
| 8 | 4.83 | — |
| 12 | **5.12** | — |

It is not a loss. The second lap adds **latency**, not work, and latency is what concurrency hides. By
twelve requests in flight the split version is faster than the unsplit one ever was. On this PC that is
a curiosity; on two Zynq boards it is the difference between running the model and not running it.

---

## Verified against the reference, byte for byte

The whole distributed stack — layer-range loading, cost-balanced splitting, the vocabulary-split head,
disk-backed embedding lookup, the two-lap protocol, per-stream KV caches and batched prefill — produces
**exactly** what one process produces:

```
Write a function that reverses a linked list in place. The function should take the head of
the linked list as an argument and return the new head of the reversed list.
```

Thirty tokens, identical text from one process and from four nodes with the model split across them.
That is the only check that catches every one of those pieces at once. A wrong vocabulary boundary, a
mis-indexed KV cache or a stale buffer would all still produce fluent English.

---

## What is still unmeasured

- **Four processes on one machine is not four machines.** Eight shared cores, so every per-node time
  includes contention and prefill cannot scale.
- **The hop cost was measured on loopback**, 30 µs. The two-Luckfox test over USB networking costs
  nothing and still has not been run.
- **The hop cost in this document is loopback.** Between two real machines it is 148 µs for a small
  message and 437 µs for an activation, measured in `hop_bench` — ten times the loopback figure, and
  still under half a percent of a stage.
