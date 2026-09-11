# 33 — A 30B mixture-of-experts, and what it actually costs the unit

*Written 2026-09-10, then rewritten the same day after the desktop numbers were thrown out.*

> **EVERY SPEED FIGURE IN THE FIRST VERSION OF THIS DOCUMENT CAME OFF A PC AND HAS BEEN REMOVED.**
> A PC is not a node in this machine. It has 47.8 GB of RAM on one bus, vector units no board here
> has, and an NVMe drive 19 times faster than anything the unit owns. A number measured on it is not
> a number about BENCH ONE, and quoting one is how a plan gets built on a lie.
>
> What survives below is what transfers: properties read out of the model file, properties of the
> model's own router, and arithmetic against storage rates **measured on a board**.

---

## 1. What the runtime gained

`model_q.c` handled one architecture shape. Qwen3 breaks three assumptions in it, and all three are
now handled:

- **head_dim comes from the file.** Qwen3-30B has dim 2048 and head_dim 128 across 32 heads, so the
  query projection is 2048 in and 4096 out. Deriving head_dim as dim over heads gives 64, which
  reads half the heads and writes past the end of the buffer.
- **Per-head query and key RMSNorm.** Qwen3 normalises within each head before the rotation. Leaving
  it out does not crash and does not look wrong. It produces fluent text that is a different model.
- **The router and the expert stack.** 128 experts a layer, 8 fire, softmax over all of them, top
  eight, renormalised to sum to one. The expert tensors are three-dimensional and the third axis is
  the expert, so a loader that reads two dimensions describes only the first expert of 128.

### Correctness, which is the only thing a PC is good for here

Against llama.cpp on the identical blob, greedy, raw prompts, four tokens deep: **eight of eight
completions match token for token.** That is a correctness check, not a benchmark. Running the same
weights through two independent implementations and getting the same tokens is the strongest evidence
available that the routing, the normalisation and the expert indexing are right.

The check also found a real defect: `route()` was writing 128 router probabilities into the attention
scratch buffer, which is sized to the context length. Every generation shorter than 128 tokens was
corrupting memory past the end of it.

## 2. What the file says, which is hardware-independent

Read out of the 18.6 GB blob by `moe_shape.py`:

| | |
| --- | --- |
| layers | 48 |
| experts per layer | 128, of which 8 fire |
| one expert | 2.92 MB |
| all experts | 16.35 GB |
| resident regardless: attention, routers, embedding, head | 0.77 GB |
| touched by one token | 2.03 GB, 11% of the model |
| of that, experts | 1120 MB |

**Eleven per cent per token is the entire reason a 30B is worth discussing on this machine and a
dense 14B is not.**

## 3. How concentrated expert use is — a property of the router, not of any computer

Six prompts on unrelated subjects, counters reset between them, measured from the router's own
choices:

| hottest N per layer | cache needed | measured hit rate | uniform would be |
| --- | --- | --- | --- |
| 4 | 0.55 GB | 14.72% | 3.12% |
| 8 | 1.09 GB | 24.82% | 6.25% |
| 16 | 2.19 GB | 40.51% | 12.50% |
| 24 | 3.28 GB | 52.75% | 18.75% |
| 32 | 4.38 GB | 62.66% | 25.00% |
| 48 | 6.57 GB | 77.40% | 37.50% |
| 64 | 8.75 GB | 87.28% | 50.00% |

Three to five times better than uniform at every size. A cache of the hot experts works.

### The caveat, also measured

Within a single prompt it looks far better: 67% at sixteen per layer rather than 40%. That is the
trap, because one long generation stays in one mode and will show concentration whatever the router
does. Hot sets overlap only 16 to 38 per cent between subjects, so the pooled figure above is the
honest one.

## 4. What it costs on THIS machine's storage

The unit's storage, measured and estimated on its own hardware:

| | rate | source |
| --- | --- | --- |
| Luckfox SD card, cold read | 17.7 MB/s | MEASURED on the board |
| 2 TB SSD on the Zynq USB 2.0 host | 35 MB/s | estimate, unmeasured |

With 3.10 GB of memory, 0.77 GB has to be resident whatever happens, leaving 2.33 GB of expert cache,
which holds seventeen experts a layer and serves 42% of expert fetches. That leaves 648 MB a token
to come off storage.

| storage | seconds per token |
| --- | --- |
| at 35 MB/s, the SSD estimate | 18.5 |
| at 17.7 MB/s, the measured SD card | 36.6 |

**That is the real answer and it is not the one the PC gave.** On the PC's NVMe the same arithmetic
said one second a token, because that drive does 661 MB/s on random reads. The unit does not have
that drive and will not have it. Streaming experts off the unit's own storage is unusable by a factor
of about twenty.

## 5. Which means the answer is resident memory, and only that

The cache curve above is the design input. Every expert that fits in memory is one that never costs a
storage read.

| memory for the expert cache | experts per layer | hit rate | storage bytes per token |
| --- | --- | --- | --- |
| 2.33 GB, today | 17 | 42% | 648 MB |
| 4.38 GB | 32 | 63% | 418 MB |
| 6.57 GB | 48 | 77% | 253 MB |
| 8.75 GB | 64 | 87% | 143 MB |
| 17.1 GB | 128 | 100% | 0 |

At 17.1 GB of expert memory plus 0.77 GB resident, the 30B touches storage exactly once, at load.
That is the target, and it is a capacity target, not a bandwidth one.

**What this says about the build:** the path that matters is the one that buys gigabytes cheaply. Two
Zynqs with 1 GB each do not get there. A DIMM-capable FPGA board does, and it is already the most
valuable unbought item in the plan.

## 6. One result worth not hiding

Perplexity is a property of the model and its weights, so it transfers.

| model | English prose, 204 tokens | this project's C, 1024 tokens | top-1 agreement on the C |
| --- | --- | --- | --- |
| Qwen2.5-Coder-3B, dense | 7.216 | 26.148 | 41.3% |
| Qwen3-Coder-30B-A3B | 7.513 | 25.638 | 46.9% |

On prose the 30B is worse. A 30B-A3B has three billion active parameters and does not beat a dense
three billion on short English.

On code it wins where it counts. Top-1 agreement is how often greedy decoding picks the token the
file actually contains, and a completion is a run of those, so 41.3% to 46.9% is a 13.6% relative
improvement in whether a completion lands.

**The 30B is not a better model. It is a better code model.** Buy memory for it on that basis and no
other.

## 7. What is still unmeasured, and it is the part that decides everything

No 30B has run on a board. Not one. Every throughput figure for the unit in this document is
arithmetic over a measured storage rate and a measured cache hit rate, not an observation.

The missing measurements, in the order they matter:

1. **The NEON kernels on a Luckfox and then on a Zynq's own cores.** The unpacking rate of the
   processors actually in the machine. Every previous figure for this came off a desktop.
2. **The Zynq's DDR3 bandwidth.** Every capacity plan scales with it and it is an estimate.
3. **The 2 TB SSD's real rate on the Zynq's USB host.** The 35 MB/s above is a guess and the whole
   storage column moves with it.
4. **The Lyra's DDR3 bandwidth and unpack rate**, currently scaled 3.26× from a Pico.

## 8. Reproducing the parts that do not need a board

```
cd firmware/bench-one/tests && python moe_shape.py <model.gguf>
```

```
cd firmware/bench-one/tests && ./moe_route.exe <model.gguf> 40
```
