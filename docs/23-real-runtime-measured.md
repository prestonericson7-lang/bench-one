# 23 — The real runtime, and the number that changes the architecture



A 3B parameter model now runs end to end in this project's own C: weights out of the GGUF blob on
this disk, tokens out of the vocabulary in that same file, 36 real transformer layers, real text out.
No framework, no llama.cpp, no Python in the loop.

```
  The capital of France is Paris. Paris is the largest city in France and is known
```

That sentence is the verification. A Q4_K dequantizer with one bit wrong, a RoPE with the wrong
pairing, a grouped-query attention that maps the wrong key head — every one of those produces fluent
nonsense at exactly the right speed. Coherent output is the only check that catches all of them at
once, and it passed on the first run.

---

## What got built

| file | what it is |
|---|---|
| [gguf.c](../firmware/bench-one/shared/gguf.c) | GGUF reader in C: header, metadata, tensor table, Q4_K, Q6_K, Q4_0, Q8_0, F16, F32 |
| [tokenizer.c](../firmware/bench-one/shared/tokenizer.c) | byte-level BPE from the file's own 151,936 tokens and 151,387 merges |
| [model_q.c](../firmware/bench-one/shared/model_q.c) | the decoder: RMSNorm, RoPE, GQA with KV cache, SwiGLU, tied output head |
| [run_model.c](../firmware/bench-one/tests/run_model.c) | load, tokenize, prefill, generate, report engineering numbers |
| [decode_limit.c](../firmware/bench-one/tests/decode_limit.c) | separates the candidate bottlenecks, old unpacking and fused side by side |
| [gguf_dot.c](../firmware/bench-one/shared/gguf_dot.c) | the fused integer dot: Q4_K, Q6_K, Q8_0, no float round trip |
| [fast_path.c](../firmware/bench-one/tests/fast_path.c) | proves the fused kernel exact, then times it against the old one |

Weights stay quantized in memory and a row is unpacked at the instant it is multiplied.
Dequantizing the model once at load would be simpler and would turn 1.8 GB into 12 GB, which is more
memory than the entire machine being built has.

---

## THE FINDING, and the correction that followed it

The project rested on decode being memory bound. Measured on this desktop, same process, same eight
threads, it is not:

| measured | rate |
|---|---|
| memory read, sum only | 29.4 GB/s |
| unpack by writing floats to memory | 5.2 GB/s |
| **unpack fused, integer dot, no round trip** | **10.6 GB/s** |
| float dot, data already in cache | 35.4 G MAC/s |
| real decode, float path | 2.28 GB/s |
| real decode, fused path | 7.60 GB/s |

**The first version of this document claimed a six-fold gap and was wrong.** It measured a loop that
wrote 256 floats to memory per 144-byte block so that a dot product could read them back once each.
That is a bad loop, not a property of 4-bit weights, and attributing it to the format flattered the
FPGA with slowness that belonged to the software.

`gguf_dot.c` does the same arithmetic reassociated. A Q4_K weight is `d*sc*q - dmin*m`, so over a
32-weight sub-block the sum becomes `d*sc * sum(q_i x_i) - dmin*m * sum(x_i)`: two floating-point
multiplies per 32 weights instead of one per weight, with an integer multiply-accumulate running
straight out of the nibbles. The activation is quantized to int8 once per matrix, with a scale every 32
elements.

| | Q4_K | Q6_K | real 69/31 mix |
|---|---|---|---|
| old | 5.17 GB/s | 5.27 GB/s | 5.20 GB/s |
| fused | 15.69 GB/s | 6.11 GB/s | 10.55 GB/s |

End to end on the same prompt, the runtime went from **1.28 to 4.25 tokens per second**, and the output
was identical token for token.

### Proving it before believing it

A faster kernel that is subtly wrong is worse than a slow one, and the first correctness test was not
good enough to tell the difference. It used a realistic activation and reported 6% mean error, which
cannot distinguish a broken kernel from a correct kernel paying the expected cost of 8-bit
activations. Two questions need two tests.

1. **Is the kernel right?** An activation chosen so int8 quantization is exact: whole numbers, with
   each 32-block forced to contain a 127 so its scale comes out as exactly 1.0. Any error left is the
   kernel's. Result: **5.89e-08 worst case over 1280 rows of five real tensors.** Both unpackers are
   correct.

   That test also found a real defect. `ffn_down` at 11008 columns came back at 3.5e-04, which read as
   a kernel bug and was not: a dot product of 11008 random signed terms nearly cancels, and a float
   running total over the 344 sub-block contributions lost that much precision. One double accumulator
   per row, free against the integer work beneath it, took it to 1e-7.

2. **What does the approximation cost?** The same kernel on a hostile activation, one element in 64 at
   20x, gives a few percent per row. That number is a poor measure, because the relative error of a
   heavily cancelled sum is dominated by the cancellation. The measure that counts is whether the
   model changes, and it does not: identical text from both paths.

## What survives, and why it still favours the FPGA

2.8x, down from the 6x that was claimed. A desktop with AVX2 still cannot unpack as fast as it can
read, and every processor in this machine is weaker at it than that one.

An FPGA has no unpacking cost at all. The high nibble and the low nibble are different wires. The
verified core at 64 lanes was measured at 71% duty against a throttled 2.5 GB/s feed, which is
2290 MB/s of weights actually consumed, so on fabric the bottleneck lands back on memory where this
architecture wants it.

The planner now carries two rates per node and the slower one wins. The same two boards, twice:

| | slowest stage | decode |
|---|---|---|
| two Zynqs, fabric does the arithmetic | 440 ms | 2.27 tok/s |
| two Zynqs, Cortex-A9 cores do it | 6,540 ms | 0.15 tok/s |

**About 15x, not 31x.** Still decisive, and still enough to rule out the ARM cores being the compute
rather than the traffic control. The number in `plan.py` and the number here moved together, because a
measurement that only updates the flattering half of a document is not a measurement.

## Where the honest comparison sits

**The desktop is now faster than the prediction for two Zynq boards: 4.25 against 2.27 tokens per
second.** Before the fused kernel the comparison ran the other way, and the earlier version of this
document quoted it that way. Fixing the software reversed it.

That is worth stating plainly rather than burying, because it says what the architecture is and is not
for. It is not a way to beat a desktop at a model the desktop already holds. Ollama on this machine
puts most of a 3B model on the RTX 2080 Super and is quicker than either figure above. The real
comparisons are:

- **Per watt.** Two Zynq boards draw about 10 W together against this desktop's few hundred, which is
  roughly 13x the tokens per watt even while losing on absolute speed. That figure needs a meter on
  both ends before it is quoted as measured.
- **Per dollar of capacity.** 428x gap, measured earlier, unchanged.
- **Scaling.** The desktop is finished at 48 GB. Adding boards adds memory AND unpacking throughput in
  the same step, which is the property no single machine has. The 3B model is the wrong test for that;
  a model that does not fit one machine is the right one.

---

## What is now measurable that was not

Two ARM binaries are cross-compiled and waiting on hardware already owned, costing nothing:

- `tests/decode_limit_arm` — turns the Luckfox unpacking estimate (66 MB/s) into a measurement. It is
  the most load-bearing estimate left, because it decides whether a Luckfox can ever carry weights.
- `tests/run_model_arm` — the whole runtime on a Luckfox, reading the model off its SD card.

The Zynq DDR3 figure of 2500 MB/s is still an estimate, and the boards are weeks out.

---

## What is still missing

- **`bench_layer.h` is now probably obsolete.** It specified a synthetic INT8-activation,
  INT4-weight layer, and `gguf_dot.c` does that job on the real Q4_K and Q6_K formats instead. Writing
  the synthetic version would measure a format no model on this machine uses. Deleting it is a
  decision to take deliberately rather than by neglect.
- **Prefill as matrix-matrix.** Prefill currently runs one token at a time, so it measures 1.40
  tok/s and reuses nothing. Batching the prompt turns it compute bound, which is the half a GPU wins
  and the half that decides time-to-first-token.
- **Pre-tokenizer exactness.** The chunker implements GPT-2's rules directly rather than its Unicode
  regex; identical on ASCII, possibly different where scripts mix. Round-trip is exact on all five
  test strings, which is the property that must always hold.
