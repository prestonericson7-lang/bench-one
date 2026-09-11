# 26 — Prefill is a different machine from decode, and now both are measured



Decode reads every weight to produce one token, so it is bound by memory and unpacking. Prefill
processes a whole prompt, and every position needs the *same* weights, so a row can be unpacked once
and used n times. That turns matrix-vector into matrix-matrix and moves the bottleneck from memory to
arithmetic.

The project asserted that from the start. It is now measured at both ends.

---

## The result

| prompt | prefill |
|---|---|
| 5 tokens | 5.52 tok/s |
| 45 tokens | 10.40 tok/s |
| 180 tokens | 10.49 tok/s |

It plateaus at 10.5 tok/s, and the plateau is the point: that is **32.5 G MAC/s against the 35 G MAC/s
this host reaches on a float dot with data already in cache.** 93% of its arithmetic peak. Prefill has
stopped caring about memory entirely.

Decode on the same machine is 4.3 tok/s. So prefill is **2.4× decode** per token, and they are limited
by completely different things.

| | limited by | per token |
|---|---|---|
| decode | memory and unpacking, 1833.9 MB read | 232 ms |
| prefill at 180 tokens | arithmetic, 10.2 MB read | 95 ms |

The weight traffic fell 180-fold. The time only fell 2.4-fold, because the arithmetic became the wall.

---

## The trap: batching made it slower before it made it faster

Worth recording, because the failure looked like the idea being wrong rather than the loop being wrong.

| version | prefill on a 45-token prompt |
|---|---|
| one token at a time | 4.98 tok/s |
| batched, obvious inner loop | **2.91 tok/s** |
| batched, 4 positions at a time | 8.87 tok/s |
| batched, 8 positions at a time | 10.40 tok/s |

The obvious loop processes one position per pass over the unpacked row, which reloads `row[c]` for
every single multiply. It reached 9 G MAC/s against the machine's 35, and a batched prefill that read
the weights 45 times less often came out **slower than not batching at all.**

Holding eight accumulators means the weight is loaded once and used eight times, and eight independent
sums give the pipeline something to overlap instead of one serial dependency chain. Eight accumulators
plus the broadcast weight is nine of AVX2's sixteen vector registers, which leaves room to unroll.

3.6× from restructuring a loop, with no change to the arithmetic performed.

---

## What this settles about the architecture

The brief has two small GPUs in it, and this is the number that justifies them.

A 2080 Super does on the order of 5500 G MAC/s against this CPU's 32.5. Prefill is now pure arithmetic,
so that is roughly **170× on prefill** for a part already in the machine. Decode would gain nothing from
it, because decode is bound by reading 1.8 GB and the GPU has 8 GB of VRAM at best.

So the split is not a compromise, it is the right allocation of two different resources:

- **GPUs do prefill.** Compute-bound, matrix-matrix, and a GPU is 170× the CPU at it.
- **Distributed RAM does decode.** Memory-bound, matrix-vector, and capacity is what it needs.

That also fixes the worst usability problem. Time to first token on a 180-token prompt is 17.2 s here,
against 41.8 s one token at a time. On a GPU doing the prefill it would be under a second, and the
distributed side would take over for generation.

---

## Still to do

- **`pipe_model` still prefills one token at a time through the ring.** The same batching applies, with
  chunks capped at 32 positions so one message stays inside the 256 KB payload limit.
- **The fused integer kernel is deliberately not used for prefill.** Unpacking a row once and reusing it
  n times beats unpacking it n times inside a fused kernel, so prefill uses the float path on purpose.
  A fused kernel that took n activations at once would beat both, and does not exist yet.
- **Attention in prefill is still per-position.** It is a small share at these lengths and becomes the
  dominant cost somewhere past a thousand tokens, because it grows with the square of the prompt.
