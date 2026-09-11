# 29 — A quality yardstick, and the constraint nobody had costed

Two pieces of research from the night of 2026-09-09. One found a limit the planner was blind to. The
other built the tool needed to judge any approximation, and immediately used it to stop a day of wasted
work.

---

## 1. The KV cache is bigger than the model

The planner placed layers and the output head and **never counted the KV cache at all**. Weights are
fixed; the cache grows with every token of conversation and again with every conversation held at once.

For Qwen2.5-Coder 3B, at int8, 18.0 KB per position for the whole model:

| context | conversations | cache | against 2079 MB of weights |
|---|---|---|---|
| 2,048 | 1 | 36 MB | 2% |
| 32,768 | 1 | 576 MB | 28% |
| 32,768 | 4 | **2,304 MB** | **111%** |

At long context with a few users, **the cache is larger than the model**. Counted properly, the 3B model
stops fitting two boards at 32k context: it needs five boards, not two.

Grouped-query attention is the only reason it is survivable. This model caches 256 values per position
per layer instead of 2048 — eight times less than attention without it. Without GQA the cache would be
the whole story at any context worth having.

### The tension this exposes

Two requirements of this machine pull against each other, and neither was costed against the other:

- It needs **several requests in flight** or n−1 of its nodes sit idle. Measured: one request in flight
  gives 1.55 tok/s on four nodes, four requests give 5.25.
- Every extra request costs a **whole extra cache**.

Context and concurrency spend the same megabytes. `plan.py` now takes `--ctx` and `--requests`, charges
the cache to the nodes before placing a single layer, and reports how much conversation fits in what is
left. On the two boards at int8 there is room for about 23,000 positions in total — four conversations
of 5,800 tokens each, or one of 23,000.

---

## 2. Exactness checks stop working on approximations

Every check in this project until now was an exactness check: run it two ways, diff, demand a match.
That is the right test for splitting a model across four nodes, reordering a loop, or moving the
embedding lookup to disk — and it caught real bugs, including a stale buffer that produced correct text
for one token and nonsense afterwards.

It is useless for an approximation. Moving the cache from float to int8 changed the generated text on
**three prompts out of five**. Both versions were fluent; both were plausible continuations. Greedy
decoding flips whenever two logits are close, so divergence proves something changed and nothing about
whether it got worse.

So `tests/ppl.c` measures perplexity: given a real passage of prose and code, how surprised is the model
by the token that actually came next. One number, comparable between runs, lower is better.

---

## 3. What the yardstick says

Measured on 203 tokens of mixed prose and Python:

| configuration | perplexity | top-1 agreement |
|---|---|---|
| float weights, float cache | 7.241 | 63.1% |
| float weights, int8 cache | 7.233 | 62.6% |
| **fused integer weights, int8 cache** | **7.216** | **63.1%** |
| fused integer weights, **4-bit cache** | **47,432** | 3.4% |

**The fast path is free.** The fused integer kernel and the int8 cache together — a 3.3× speedup and a
4× memory saving — land at 7.216 against the full float reference's 7.241. Fractionally *better*, which
means the difference is noise. Both approximations can be used without reservation.

**Four bits destroys it.** 7.216 to 47,432, and top-1 agreement from 63% to 3.4%. The model is gone. The
cliff between 8 and 4 bits is absolute, not gradual.

That is the result the experiment was built to get. The cheap half was done first on purpose — quantize
to 4-bit levels while still storing one value per byte, measuring the *quality* of the approximation
before writing any of the packing needed to collect the *memory*. The packing would have been a day's
work for a model that cannot form a sentence. **int8 is the floor for the KV cache, and the question is
closed.**

---

## 4. A process mistake worth recording

The 4-bit result nearly came out wrong. The flag was parsed, the label was printed, and the function
that applies it was never called — so the first run reported the int8 perplexity and looked like proof
that 4 bits was free. A flag that is parsed but not applied is worse than one that does not exist,
because the test appears to pass.

The compiler said so plainly: `variable 'want_kv_int4' set but not used`. It was in a build log being
filtered for the word "error".

The root cause was a habit, not a typo. Edits were being applied by scripts that search for a string
and replace it, and **carry on silently when the string is not found**. Two changes that night failed
that way. Every patch now asserts that its target existed, or uses an editor that refuses rather than
shrugs.

---

## 5. Where this leaves the design

- **The KV cache stays int8.** Free in quality, a quarter of float, and four bits is not an option.
- **Context length is a capacity decision, not a setting.** It competes directly with concurrency, and
  the machine needs concurrency to keep its nodes busy.
- **Perplexity is the yardstick from here on.** Any future approximation — narrower activations, a
  coarser weight format, dropping a head — gets measured against 7.216 before it is believed.
- **One known bug left:** the placer packs layers onto the largest node instead of spreading them, so it
  understates what extra boards are worth. It happened to be hidden while capacity forced a spread
  across two 1 GB boards, and shows up the moment a bigger board exists.
