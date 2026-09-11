# 30 — Ten paths, and what each one is actually worth



Written overnight, 2026-09-09 into the 10th. Every path below is either backed by a measurement taken
on this bench, derived from one, or marked untested. Two of them I tested and they failed, and those
are in here too, because a list of ten ideas where all ten work is a sales brochure.

Ranked by how much evidence there is, not by how good they sound.

---

## 1. Mixture-of-experts is the shape this machine was waiting for

**MEASURED, and it is the strongest result of the night.**

Read out of a model already on this disk — Qwen3 Coder 30B A3B:

| | |
|---|---|
| model on disk | 18.6 GB |
| experts | 128, of which **8 fire per token** |
| expert weights | 17.6 GB, **95% of the model** |
| one expert's slice of one layer | **2.9 MB** |
| **read per token** | **2.1 GB** |

Two numbers change everything.

**2.1 GB per token from an 18.6 GB model.** The dense 3B reads 1.83 GB per token. So a 30B model costs
about the same per token as a 3B one. On a machine whose binding constraint is bandwidth, paying 3B
prices for 30B quality is the whole game.

**2.9 MB per expert per layer.** A dense layer is 49.2 MB and no microcontroller in the fleet can hold
one. An expert block is 2.9 MB, and a Luckfox holds four. **For the first time the small boards can
carry weights instead of just moving them.**

The catch, and it is real: routing. Eight experts fire per layer, each needing a 16 KB activation in
and out, so 48 layers costs about 12 MB of traffic per token. At the measured 18.7 MB/s that is 0.65 s
a token of pure network. The fix is to group many experts of the same layer onto one node so the
traffic collapses, and that is a placement problem, not a physics problem.

**Next step:** teach `plan.py` about experts and find the grouping where routing traffic stops
dominating.

---

## 2. Prefill on the GPU, decode on the stack

**MEASURED at both ends.**

| | limited by | per token |
|---|---|---|
| decode | memory and unpacking, 1834 MB read | 232 ms |
| prefill, batched | arithmetic, 10.2 MB read | 95 ms |

Batched prefill hits **93% of this desktop's floating-point peak**, which makes it pure arithmetic —
the one thing a GPU is overwhelmingly better at, roughly 170× this CPU. Decode reads 1.8 GB per token
and gains nothing from a GPU that has 8 GB of VRAM.

They are two different machines wearing one name. The two small GPUs in the brief have a clear job and
it is not the job the stack does.

---

## 3. Capacity that grows in $60 steps, at 0.05% cost per step

**MEASURED.** Sum of every node's compute 645.1 ms, wall clock 645.4 ms. The network costs 0.3 ms.
One activation per hop is 8 KB against 49.2 MB of weights per layer — five thousand to one.

A desktop is finished at 48 GB: soldered memory controller, fixed core count, no upgrade at any price.
This grows by plugging in another board, and the measured overhead of doing so rounds to nothing.

That is the difference between a machine with a ceiling and one without.

---

## 4. The FPGA as a memory tier that does its own arithmetic

**MEASURED by synthesis.** 512 lanes of the INT4 core is 37% of a $60 XC7Z020 and unpacks 25.6 GB/s.
This entire eight-thread desktop manages 10.55.

The interesting version is not "replace the GPU". It is **bolt this onto a GPU box.** A GPU is
compute-rich and memory-poor; VRAM is the most expensive memory in the building. An FPGA with a DIMM
socket is memory-rich and, at 256 lanes for 19% of the chip, no longer compute-poor. It holds the
weights a GPU cannot fit and does the 4-bit arithmetic locally instead of shipping bytes.

**Where it stands:** the lane counts are synthesized, the 2500 MB/s DDR3 figure is still an estimate,
and the DIMM board is not bought.

---

## 5. Training with no dataset at all

**BUILT AND RUNNING TONIGHT.** `tests/fractal_train.c`: a small network learns the Mandelbrot set from
an equation. Loss fell 0.0080 to 0.0024 over 1.5 million samples at 11,000 samples a second, and not
one byte of training data was read from disk.

Why it matters here specifically: this machine has 2.2 TB of storage at 17 MB/s — capacity without
bandwidth. Feeding a dataset to 208 nodes would be the bottleneck before any arithmetic happened. A
target computed from a formula removes the problem rather than working around it.

And the model is 35,000 parameters, so **every node holds all of it.** That flips the parallelism from
pipeline to data-parallel, which is the arrangement many small nodes are actually good at.

The fractal is also a capacity meter that never saturates: a fixed dataset eventually gets a perfect
score and stops telling you anything, while the boundary has detail at every scale forever.

---

## 6. Many models resident at once, with no swapping

**MEASURED, partly.** Cold start is 59 seconds to fill RAM from the SSD. A GPU with 8 GB holds one
mid-sized model and swaps to change; 208 independent nodes hold 208 things and swap never.

The obvious use is many small specialised models — one per language, per codebase, per user — answering
at once. The measurement that supports it is the load time it avoids. The measurement that would prove
it is a many-model serving test, which has not been run.

---

## 7. The stack as a context store for GPUs — on capacity, not speed

**PARTLY DISPROVEN. Read this one carefully.**

The idea: the KV cache is what fills VRAM, so hang cheap DRAM off a GPU to hold it. The capacity half
is solid and measured — at 32k context with four conversations the cache is **2,304 MB against 2,079 MB
of weights, bigger than the model.**

The speed half is what I tested tonight, and it failed. I expected attention over cached context to
suit the small boards, because the cache is plain int8 with no nibbles to unpack, and unpacking is the
thing they are worst at. Measured on this desktop:

| | throughput |
|---|---|
| attention over cache | **3.0 GB/s** |
| 4-bit weight unpacking | 10.55 GB/s |

Attention is **3.5× slower**, not faster. The reason is arithmetic density: a packed byte holds two
4-bit weights and yields two multiply-accumulates, while a cached byte yields one. Per byte of memory
read, weights are twice as productive.

So this path survives as a capacity argument and dies as a throughput argument. Stated the other way
round it would have been the most exciting thing on the list and it would have been false.

---

## 8. Serving many requests, which this machine has no choice about

**MEASURED.** On four nodes: 1.55 tok/s with one request in flight, 5.25 with four. With a single
conversation only one node ever works, because the next token cannot start until the last one exists.

This is not a marketing feature, it is a constraint. Utilisation is one over the node count until
several independent requests are moving. It makes the machine a multi-request server and it makes a
single-user chatbot the wrong thing to build on it.

The tension worth knowing: every extra concurrent request costs a whole extra KV cache, so context
length and user count spend the same megabytes.

---

## 9. Failure that costs a fraction instead of everything

**DERIVED, not measured.** A GPU dies and inference stops. A node dies and 1/n of the layers go with
it — and since every node computes the layer split from the same file with no coordinator, a fleet that
notices a missing node can recompute the split and carry on slower.

The pieces exist: the split is deterministic, there is no master, and the protocol carries the sequence
position rather than trusting either end to count. Nothing has been tested by pulling a cable, which is
exactly how it should be tested.

---

## 10. Tokens per watt, for work where latency does not matter

**ESTIMATED, and it needs a meter before anyone repeats it.** Two Zynq boards draw about 10 W together
against this desktop's few hundred, which is roughly 13× the tokens per watt even while losing on
absolute speed.

Overnight batch work — indexing a codebase, generating tests, summarising a corpus — does not care that
a token takes 400 ms. It cares what the electricity costs. That is the workload where a slow, cheap,
cool machine wins outright, and it is the least glamorous item on this list.

---

## What I tested and could not make work

Recording these so nobody re-runs them hopefully.

- **KV cache at 4 bits.** Perplexity 7.216 to **47,432**, top-1 agreement 63% to 3.4%. The model cannot
  form a sentence. The cliff between 8 and 4 bits is absolute. int8 is the floor and the question is
  closed.
- **Attention as the small boards' workload.** 3.5× slower than weight unpacking on the same machine.
  See path 7.

## What would move the most, in order

1. **Teach the planner about experts** and find the grouping where MoE routing traffic stops dominating.
   Path 1 is the biggest prize on the list and the only thing between it and a build is placement.
2. **Measure the Zynq's real DDR3 bandwidth** the day the boards land. Every number here scales with it.
3. **Run the 30B MoE** through the existing runtime. The loader already reads the file; the routing does
   not exist yet.
4. **Put a meter on the wall** and turn path 10 from an estimate into a number.
