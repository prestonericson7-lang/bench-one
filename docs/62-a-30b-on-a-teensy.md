# 62 — A 30-billion-parameter model on a Teensy

*2026-10-16. The owner's ask: "load a bigger model, store the important uses on the RAM for fast use, the rest for
recall on the SD card ... do it with the 64gb."*

## The idea, and the part of it this board cannot do

The ask has two halves. "A bigger model, the rest on the card": yes, and this document is how. "The important parts in
RAM for fast use": not on this board, and the reason is measured, not assumed:

| store | size | how it reads |
|---|---|---|
| the card (SanDisk Ultra 64 GB, ADMA2 into a ring) | 59.6 GiB | **44 MB/s**, by DMA, the CPU free (doc 56) |
| PSRAM, the 8 chips | 64 MB | 2.9–9.5 MB/s a chip (boot log 2026-10-16), by the CPU |
| on-chip RAM | 1 MB | already the arithmetic's buffers; one expert of the model is 2.92 MB |

The card is the fastest place this board can hold weights. A weight cached in PSRAM would come back slower than from the
card, and cost CPU time the card does not. So the model stays on the card, all of it (the owner's own rule, doc 54), and
the PSRAM keeps the model's working memory: the attention cache.

The "keep the important part close, recall the rest" idea is what a **mixture-of-experts** model does by itself. Its
feed-forward is 128 experts a layer of which a router picks 8 per token: 11% of the model is touched per token (doc 33).
The rest stays on the card until a token needs it.

## The model

Qwen3-Coder-30B-A3B-Instruct, Q4_K_M, 18,556,688,736 bytes — already on the PC (Ollama's `qwen3-coder:30b`), already
proven against llama.cpp in `model_q.c` (doc 33).

| | Qwen2.5-Coder-3B (until now) | Qwen3-Coder-30B-A3B |
|---|---|---|
| parameters | 3.09 B | 30.53 B |
| layers, width | 36, 2048 | 48, 2048 |
| attention | 16 heads / 2 kv | 32 heads / 4 kv, q 4096 wide, per-head q/k norm |
| feed-forward | dense, 11008 | 128 experts x 768, 8 picked per token |
| **read from the card per token** | 1.93 GB | **1.92 GB** (1,919,597,696 bytes; the host build counted 1,830.7 MiB a step) |
| attention cache in 64 MB PSRAM | 2,608 positions | 1,126 positions, one chip spare |

Ten times the parameters for the same bytes a token: of the 30B's 16.35 GB of experts only the 8 picked per layer
(1.08 GB) come off the card, beside its attention (0.51 GB), routers (50 MB) and output head (255 MB, the same size
as the 3B's).

## What the runtime needed (`shared/tl_core.c`)

- **The architecture** `qwen3moe`: three-dimensional expert tensors (expert e's rows at e x 768 of the gate and up
  stacks, e x 2048 of the down stack), the router (an F32 2048 x 128 matrix), per-head q/k RMSNorm before the rotation.
- **The router** exactly as `model_q.c`: a plain float dot per expert, softmax over all 128, the top 8 (first index on
  a tie), renormalised to sum to one.
- **One card pipeline per layer** over every picked expert — gate rows, up rows, down rows — each block of rows read once
  for every position that picked that expert. The pipeline works block *i* while block *i+1* comes off the card, and
  block *i+1* may already belong to the next expert, so every job carries its own expert's context.
- **The experts summed in ascending expert number.** `model_q.c` added the eight weighted outputs in the order they were
  picked. A batched pass feeds four prompt positions and reads each expert once for all of them, in expert order — so each
  position's sum arrives in expert order, and the only way that equals the reference to the bit is for the reference to
  add in the same order. `model_q.c` now does (its router dot is also kept in plain order unless asked otherwise).
- **A query wider than the buffers** (4096 over a 2048 residual): a position takes two rows of the batch arrays, so a
  pass feeds 4 positions instead of 8, and nothing grows. The per-token path runs through the batched pass with one slot,
  so the expert code exists once.
- **Room on the chip**: the model table (21 KB at 48 layers) moved to OCRAM — with its "open" flag in DTCM, because the
  Teensy's startup zeroes only DTCM and the table is garbage until the model opens — paid for by turning the `::multi`
  token arrays into one pool sized to the cache (32 KB to 12 KB). The opening code moved to flash: the expert code had
  pushed the instruction memory 3 KB past a 32 KB bank edge, which costs the stack a whole bank (doc 56's trap). After:
  stack free 30,784 bytes, up from 20,096 this morning.

## The proof, on the PC (correctness only)

`tests/tl_host.exe` (the core, weights from the file every token, the PSRAM a separate store) against `tests/tl_ref.exe`
(`model_q.c`), on the 30B:

- the France chat question: **32 of 32 step lines identical** — ids, both logits to nine digits, the second-best token;
- the tokenizer, built by the core from the 30B's file, against `shared/tokenizer.c`: identical on 31,443 corpus lines
  and 2,000 fuzz records;
- per token, prompt-batched (4 positions a pass, the expert groups) and speculative lookup: identical on every prompt;
- the 3B unchanged: identical per token and batched (the full regression, below).

- the full quick regression on the 30B (`verify_pc.py`, MODEL= the 30B; `20261016-143912-psram_llm-pcverify`): **ALL
  IDENTICAL** -- tokenizer, forward per token and batched, lookup, the output head at the last position only, prompts
  answered together (4 at a time, shared openings off and on), a PSRAM bank killed mid-run and recovered, and the M7's
  own SXTB16/SMLAD kernels emulated on the PC.

**Against llama.cpp** (the independent implementation, Ollama's `llama-server` on the same file, greedy, the same
prompt ids, 16 tokens; `20261016-150316-vs-llamacpp-moe`): **6 of 8 answers identical token for token.** The two
partings, each checked against `model_q.c`'s float path (weights dequantized, float cache -- the yardstick, doc 51):

| prompt | parts at | margin ours / llama.cpp | float path picks |
|---|---|---|---|
| "The capital of France is" | token 14: " Italy" (ours) or " Luxembourg" | 0.205 / 0.464 | " Luxembourg", 22.50 against 22.19 -- **llama.cpp's** |
| "Name three primary colors." | token 15: `**\n` (ours) or `**` | 1.451 / 0.938 | `**\n`, 36.13 against 35.25 -- **ours** |

One each. At the near-tie (prompt 0, our margin 0.2) the int8 activations tipped our path off the float answer -- and
the reference as it was before the summation-order change tips the same way (" Italy", 22.87 against 22.69), so that
is the int8 path's own behaviour at a near-tie, not something this change introduced. Where the margin was not small
(prompt 4), llama.cpp is the one that left the float answer.

**What the summation order does change, measured.** On prompt 0 the old (pick-order) and new (expert-order)
references choose the same token at all 21 steps, but their top logits differ by a median 0.14 and at most 1.51 --
far more than "the last bits". A mixture-of-experts amplifies a one-ulp difference: it can flip an int8 rounding or a
near-tied router choice somewhere in 48 layers, and the logits move at the 0.1 level. (For scale: the float path's
top logit at step 16 is 33.56, the new order's 34.39, the old order's 35.90.) Neither order is the exact sum; which
is closer to float on average is what perplexity measures (`tests/ppl.c`, the fused int8 path, int8 cache, the first
767 tokens of doc 33 as the passage; the same text for both):

| summation order | perplexity |
|---|---|
| new: ascending expert number (what the board computes) | **21.845** |
| old: the order the experts were picked | 21.965 |

0.55% lower with the new order -- the size of difference any regrouping makes, so read it as "no worse", not as an
improvement. The order a batched pass needs costs the model nothing measurable.

## How many experts a pass over several positions reads (the router, measured)

A batched pass reads the attention, routers and head once for all its positions, but every expert any of them picked.
How much consecutive positions share is a property of the model's router, so it transfers from the PC unchanged:
`tests/moe_overlap.exe` fed the eight regression prompts and 32-token greedy answers through the reference, took each
position's picks from the expert-hit counters, and counted distinct experts over windows of the answers:

| positions in one pass | distinct experts a layer (mean) | of the most it could be | card bytes of the pass / one position's |
|---|---|---|---|
| 1 | 8.00 | 100% | 1.00 |
| 2 | 12.59 | 79% | **1.32** |
| 3 | 16.24 | 68% | 1.58 |
| 4 | 19.34 | 60% | **1.80** |
| 6 | 24.41 | 51% | 2.15 |
| 8 | 28.36 | 44% | 2.43 |

(bytes: 0.84 GB read once -- attention, routers, head -- plus the experts' 1.08 GB in proportion.) Two consequences:

- **The prompt**: a 4-position pass reads 1.80x one position's bytes for four positions -- 0.45x per position, 2.2x less
  card than feeding the prompt one token at a time. Its arithmetic is four positions' worth, about as long as its read.
- **Speculative decoding** on this model pays only past its cost: verifying one guess costs 1.32 passes, so it wins when
  the guess is right more than 32% of the time; verifying three costs 1.80 and needs 0.8 accepted tokens a pass. Weaker
  than on the dense 3B, where a guess costs almost nothing extra -- but not nothing.

**Speculative lookup on the 30B, simulated on its own answers** (`alpha_sim.simulate` -- the board's `::lookup` rule
-- on the reference's greedy answers to the eight prompts, 64 tokens each, 410 tokens after the first; a pass over P
positions costing the card bytes above at 44 MB/s, 43.6 / 57.6 / 68.9 s for 1 / 2 / 3, and 83 s for 4, where four
positions' arithmetic outlasts the read):

| guesses a pass (`::lookup K`) | match | guesses right | decode time vs one token a pass |
|---|---|---|---|
| 1 | last 4..1 tokens (`::lookup 1 1`) | 82 of 147 (56%) | **1.093x** |
| 2 | 4..1 | 105 of 243 | 1.092x |
| 3 | 4..1 | 111 of 344 | 1.018x |
| 1 | 4..2 | 54 of 84 | 1.071x |

One guess a pass, from as short a match as one token, is the setting: 9% faster, exact (a wrong guess costs its
experts and nothing else). More guesses do not pay on a mixture of experts -- every extra position drags in its own
experts. The board keeps lookup off by default; `::lookup 1 1` turns it on.

**What the jumps cost the card stream.** The board reads by ADMA2 into a ring that keeps one card stream open while
reads follow each other; a read that jumps restarts it (a CMD12 and a CMD18). `tl_host`'s read census now models
exactly that rule (`TL_READCENSUS=1`, "ring streams opened"); on the 3B it predicts **217 streams a decode pass --
the board's own count for the same pass (`CARD step=36 fifoirq opens=217`, `20261016-164101`)**. On the 30B it
predicts **1,393 a pass**: per layer the router, three slices of each of eight experts, the attention matrices and the
small norms (each a plain read, which closes the stream). What a restart costs on this card is a board number; at
1 ms it is 1.4 s a token, at 3 ms 4 s. Measured below.

*(Board results follow.)*

## Going bigger on a 128 GB card (planning, not built)

The same architecture one size up is Qwen3-235B-A22B (`qwen3moe`: 94 layers, width 4096, 64 query heads, 128 experts of
1536, 8 picked, 22 B parameters touched per token). Its quantizations, sized from the Hugging Face file listing of
`unsloth/Qwen3-235B-A22B-Instruct-2507-GGUF` (2026-10-16), against the ~119 GiB a 128 GB card holds:

| quantization | file | fits | card bytes a token (22 B x the file's bits a weight) | at the card's 44 MB/s |
|---|---|---|---|---|
| Q2_K | 79.8 GiB | yes | ~8.0 GB | ~3 min a token |
| UD-Q3_K_XL | 97.0 GiB | yes | ~9.5 GB | ~3.6 min |
| Q3_K_M | 104.7 GiB | yes | ~10.5 GB | ~4 min |
| IQ4_XS | 116.9 GiB | too tight | | |
| Q4_K_M | 132.4 GiB | no | | |

What the runtime would need: a 4096-wide residual (the query trick of this document, applied to the residual: a
position in two rows, so a pass feeds 2 positions); 94 layers in the model table (40 KB at today's 24 bytes a tensor --
more than the OCRAM left, so the table must shrink); the Q3_K and Q2_K fused kernels (the board has Q4_K, Q6_K and Q8_0);
and an attention cache of ~100 KB a position, so ~550-650 positions in the 64 MB of PSRAM. Arithmetic, not measured:
the card makes it minutes a token, not seconds -- the 30B is the model that is fast on this board.
