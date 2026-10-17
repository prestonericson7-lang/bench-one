# 63 — An 80-billion-parameter model on a Teensy

*2026-10-17. The owner's ask: "ground breaking work further ai research" — and, asked whether to add two more PSRAM
chips for a full-precision state: "No, use 16-bit state".*

## Why this model

Doc 62 put a 30B on the board at the speed of the 3B, because a mixture of experts reads only the experts a token
picks. The next step up on the 64 GB card is not a bigger transformer of the same kind — the next of those (Qwen3-235B)
needs the 128 GB card and minutes a token. It is a different kind of model:

**Qwen3-Next-80B-A3B-Instruct** (Q4_K_M, 48,506,100,512 bytes, sha256 `f73551b5…e280fd`, `unsloth/Qwen3-Next-80B-A3B-Instruct-GGUF`).

| | Qwen3-Coder-30B-A3B (doc 62) | Qwen3-Next-80B-A3B |
|---|---|---|
| parameters | 30.53 B | **79.67 B** |
| layers | 48, all attention | 48: **36 linear attention** (gated DeltaNet) + 12 gated full attention |
| attention | 32 heads / 4 kv, head 128 | 16 heads / 2 kv, head 256, rotary on 64 of 256 dims, a sigmoid gate per output |
| feed-forward | 128 experts x 768, 8 picked | **512 experts x 512, 10 picked**, plus a shared expert on every token |
| new formats | — | Q5_K (the linear layers' input projection, the shared expert), BF16 (one gate row a layer) |
| read from the card per token | 1.92 GB | **2.43 GB** |
| what PSRAM holds | the attention cache of 48 layers | 36 recurrent states (fixed size) + the cache of 12 layers |
| context in 64 MB of PSRAM | 1,126 positions | **1,147 positions**, one chip spare |

Three quarters of its layers have **no attention cache at all**. A gated DeltaNet layer keeps, per value head, a
128 x 128 matrix that every token updates — a memory of fixed size, whatever the length of the text. On this board
that is the property that matters most: the attention cache is what PSRAM holds, and here only 12 of 48 layers need one.

## What a linear-attention layer does

Per token, per value head (32 of them), on the state S (rows j = values, columns i = keys), as llama.cpp computes it
(`qwen3next.cpp`, `delta-net-base.cpp`):

    q, k, v  = a causal convolution (4 taps, per channel) over the last inputs, then SiLU; q and k l2-normalised
    g        = softplus(alpha + dt_bias) * A          (A < 0: a decay rate per head)
    beta     = sigmoid(b)                              (how strongly this token writes)
    S        = S * exp(g)                              forget a little
    d        = beta * (v - S k)                        what this token says, less what S already predicts
    S        = S + d kᵀ                                write it in along k
    o        = S q                                     read along q
    out      = RMSNorm(o) * SiLU(z)                    gated, then the output projection

It is the delta rule — a learned associative memory, updated by its own prediction error — and each row of S depends
only on itself, so the row is the unit of work: on the Teensy a block of 64 rows comes out of PSRAM, takes every
position of the pass in order, and goes back.

## The state, at 16 bits

36 layers x 32 x 128 x 128 values is 72 MB at fp32 — more than the 64 MB of PSRAM. At 16 bits it is 36 MB. The owner
chose 16 bits over adding chips; whether 16 bits costs the model anything is measured, not assumed
(`tests/ppl_state.c`: the same model several times in lockstep over one 1,023-token passage of prose and C, the state
rounded after every token, each variant scored by perplexity and by its KL divergence from the fp32 state per token).

**1,023 tokens, the fp32 state as the yardstick** (every variant on the fused int8 path the Teensy computes):

| state | perplexity | KL from fp32, mean | KL, last quarter | worst token | greedy pick = fp32's |
|---|---|---|---|---|---|
| fp32 | 2.3815 | 0 | 0 | 0 | 100% |
| **IEEE half (fp16)** | 2.3843 (+0.12%) | 9.8e-4 | 8.1e-4 | 0.071 | 99.7% |
| bfloat16 | 2.3825 (+0.04%) | 1.2e-3 | 1.3e-3 | 0.156 | 99.4% |

The error does not grow as the state ages: the last quarter's KL is no larger than the mean.

**511 tokens, against FLOAT** — what a KL of 1e-3 is worth. The board's int8 activations are themselves an
approximation of float, accepted since doc 33; this puts the state's rounding beside it:

| variant | perplexity | KL from float | KL from int8 + fp32 state | worst token |
|---|---|---|---|---|
| float, fp32 state | 3.9023 | 0 | 1.8e-3 | |
| int8, fp32 state (the accepted approximation) | 3.8816 | 2.20e-3 | 0 | 0.62 |
| **int8, fp16 state (what the board runs)** | 3.8811 | **2.17e-3** | 1.38e-3 | 0.43 |
| int8, int16 with a float scale per row | 3.8900 | 3.31e-3 | 1.53e-3 | 1.10 |

**Rounding the state to IEEE half adds nothing measurable to what the int8 activations already cost**: its distance
from float is the int8 path's own. The 16-bit state is free, and half is the default (`tl_set_state16(1)`,
`model_set_state16(m, 1)`).

One idea measured and not taken: int16 with one float scale per 128-value row, which keeps 15 bits relative to each
row's largest entry and should, by that argument, beat half's 11. It is the worst of the four. A decaying state's small
entries carry weight in its dot products, and a row scale set by the largest entry rounds them coarsely; half keeps
their relative precision. Measured, not argued — the argument was wrong. (Mode 3 stays in both runtimes for anyone who
wants to check: `TL_STATE16=3`.)

## Is it worth twice the time?

The three models this board runs, on the same 1,023 tokens of the passage above (one tokenizer for all three, so the
same token sequence), each on the fused int8 path the Teensy computes:

| model | perplexity | loss a token | greedy pick = the actual next token | on the board, a token |
|---|---|---|---|---|
| Qwen2.5-Coder-3B | 2.933 | 1.076 nats | 75.6% | 43.3 s, measured |
| Qwen3-Coder-30B-A3B | 2.716 | 0.999 nats | 79.6% | 44.4 s, measured |
| **Qwen3-Next-80B-A3B** | **2.384** | **0.869 nats** | **80.2%** | ~85 s projected (~70-75 with the levers below) |

The 80B takes 13% off the 30B's loss and 19% off the 3B's. The 30B was the free step (the 3B's time); the 80B is the
first step that costs time, and it buys the largest drop of the three.

## What the runtime needed

**`shared/model_q.c`** (the reference): the hybrid's layers transcribed from llama.cpp — the convolution and its
state, the l2 norm exactly as llama.cpp builds it (`rms_norm(eps/n) x 1/sqrt(n)`), the delta rule, the gated norm;
the gated attention (each head's 512 query rows are its query then its output gate); rotary dimensions from the file
(64 of 256 here; head_dim on every earlier model, which run unchanged to the bit); the shared expert added after the
routed experts' sum; Q5_K and BF16 in the loader and the kernels. The state is fp32, or rounded to 16 bits after every
token (`model_set_state16`) exactly as the Teensy stores it.

**`shared/tl_core.c`** (the Teensy): the same arithmetic for the four positions of a pass. What made it fit:

| constraint before | how it was met |
|---|---|
| OCRAM: 7 KB free | the linear layer's scratch (4 positions x 8,192 channels) **shares the attention score buffer** — a layer is one or the other, never both; the attention gates go after the scores (context cut from 3,072 to 2,048 positions for this model, below what PSRAM allows anyway) |
| the model table (48 layers of tensor records) | attention and DeltaNet tensors share one set of fields (a union; a bit per layer says which) |
| ITCM 4.9 KB under a 32 KB bank edge (crossing it costs the stack 32 KB) | everything that runs once a layer — the DeltaNet layer, the shared expert, the layout — lives in flash; the per-row kernels stay in ITCM. First build crossed the edge (stack -8.4 KB); final: 1,784 B of padding, **24,320 B of stack free** |
| Q5_K had no kernel | an M7 DSP kernel (Q4_K's nibble word plus each byte's fifth bit), single and batched, with the activation sums supplied — **bit-identical to the scalar reference** (`dot_verify_m7`) |
| PSRAM: one layer's cache per region, nothing straddling chips | a second region kind: a linear layer's 16-bit state (rows of 260 bytes: 128 values and a float row scale), a checksum per row, the convolution's window; placed first-fit from the fastest chip, then the attention caches; max context searched with one chip held spare |
| a recurrent state cannot be rewound | one sequence at a time: the core refuses a pass that does not continue the positions already fed; the board turns off prompt-cache reuse and `::lookup` for this model |

## The proof, on the PC (correctness only)

| check | result |
|---|---|
| `model_q.c` against llama.cpp (Ollama's bundled `llama-server`, CPU), 8 prompts x 17 tokens, greedy, same ids | **7 of 8 answers identical token for token**; the 8th (`const add = (a, b) =>`) parts at its first token, a near-tie on both sides (margins 0.020 ours, 0.043 llama.cpp) |
| `tl_host` (the Teensy core, weights from the file, PSRAM as eight separate banks) against `tl_ref` | **identical to the bit**, per token and with the prompt batched four positions a pass, in every state format |
| the M7 kernels (`dot_verify_m7`) | Q5_K single, presum and batched 1..8: identical to the scalar reference on every row |
| the whole 80B forward pass through the Teensy's own SXTB16/SMLAD code, emulated in C (`tl_host_m7`) | identical to the bit, per token and batched |
| all of it at once on the final build (`tests/psram_llm/verify_next.py`) | **everything identical**: the three state formats both ways, the 8 prompts both ways, both injected faults, the F16C unit, the emulated M7 — `bench-archive/20261017-034153-psram_llm-nextverify`; the 3B's `verify_pc.py` on the same build: all identical (`20261017-022717-psram_llm-pcverify`) |
| a chip failing mid-run (`TL_PS_FAULT=0:2500`: bank 0, seven layers' states, dies on a write) | its layers move, the prompt re-runs from position 0, the 5 lines printed before the fault come back identical, the answer equals the reference |
| the 3B and the 30B after every change (tl_ref and tl_host, per token and batched; `verify_pc.py`) | identical to before; the PC tools now map the model file on Windows too (they read all 45 GB into RAM before, which failed beside llama-server) |

"The capital of France is **Paris**. The capital of Germany is **Berlin**." — the 80B, through the Teensy's code.

## What it will cost on the board (arithmetic from the PC's counts, not measured)

| | per decode token |
|---|---|
| card | 2,318 MiB of weights (2.48 GB through the pipeline, with the head) — at the SanDisk's ~43 MB/s, **~57 s**; the PC's read census counts **1,825 stream restarts a token** (the 30B's 1,394, which the census predicted to within one) and 566 small plain reads (norm gains, the convolution weights' 16 KB blocks) |
| PSRAM | 41 MB read and 41 MB written (the states and convolution windows); the board reads every write back to check it, so ~123 MB of transfers: 14 states on the two quad chips (~9.7 MB/s), 22 on the single-line chips (~2.6 MB/s) — **~30 s if none of it overlaps the card** |
| compute | the delta rule and the 16-bit rounding: ~1-2 s |

A prompt pass of four positions reads 4,085 MiB (the PC's count): with 512 experts and 10 picked, four positions share
few experts, so they read ~3x one position's (the 30B's four read 1.8x). About 100 s of card a pass plus the state once,
so a 36-token chat prompt's first answer token comes after ~9 passes, roughly **18 minutes**.

So roughly **85 s a token** for an 80B, the first time it runs — twice the 30B's time for 2.6 times the parameters.
The PSRAM part is the new wall, and the first thing to measure; doc 63's sequel will say what it really is.

## Levers made ready on the PC, to be measured on the board (2026-10-17, night)

Each is a runtime switch, so one board session measures them all without a new build, and each was proven on the
PC not to change a single printed digit:

| lever | what it saves (arithmetic) | proof on the PC |
|---|---|---|
| **`::psverify 0`** — the state's and convolution window's writes without the board's read-back | the read-back is a third of the state's traffic: **up to ~10 s a token** on these chips | identical output; and a *silently* corrupted write (`TL_PS_CORRUPT`: one byte flipped, the write reported good) is caught by its row's checksum on the next read, the bank retired, the prompt re-run, every line identical. The price of the switch: a transient bad write that a read-back would have redone now costs a bank and a re-run — 68 archived board logs show no transient write correction ever |
| **`::f16test` then `::f16hw 1`** — the state's half rounding by the FPU's VCVTB instead of integer code | ~12 instructions -> 1 per conversion: **~1.4 s a token** of critical-path arithmetic | `::f16test` compares the two on every one of the 2^32 floats and 2^16 halves on the board, minutes, and only a clean run unlocks the switch. On the PC the same switch takes F16C: self-test clean (8.4 s), output identical; without a unit the switch is refused |
| the convolution window as a ring (always on) | a token writes the one tap that changed, not all three: ~64 KB less written and read back a layer, **~1.3 s a token** | identical output per token, batched, and through a bank fault |
| the fp16 rounding stores each half as it rounds it | one conversion less per state value per token (always on) | identical |

## The board session, step by step

1. The card into the PC's reader. The 64 GB card holds the 3B and the 30B; the 80B (48.5 GB) needs the 30B's room, so
   `qwen30b.gguf` and `qwen30b.tok` come off the card (the 30B stays on the PC as its Ollama blob, and can go back).
2. `python firmware/bench-one/tests/psram_llm/copy_model_to_card.py D:\start\models\Qwen3-Next-80B-A3B-Instruct-Q4_K_M.gguf
   E:\qwen80b.gguf --sha256 f73551b55e3796526c37772c96227151fa11af328802b8f9865d635104e280fd` — copied, flushed, read back
   from the card unbuffered and hashed. Then `model.txt` on the card: `qwen80b.gguf`.
3. The card into the Teensy, USB in; flash `tests/psram_llm` (build.bat upload) with the PC idle. The first open builds
   `qwen80b.tok` (8.3 MB) on the card; the boot line must say the hybrid's layout and "contiguous" or its run count.
4. Exactness and the first measurement, everything at its default:
   `python board_model_run.py --model D:\start\models\Qwen3-Next-80B-A3B-Instruct-Q4_K_M.gguf --gen 8 --prompt "The capital of France is"`
5. The levers, same prompt: `--pre "::f16test@^F16TEST bad=@900" --pre "::psverify 0"` — the step lines must still equal
   `tl_ref`'s, and the pass lines say what each saved (card, PSRAM and compute seconds are printed per pass).

**Found, not built: the routers are bfloat16 in float32's clothing.** Every one of the 50,331,648 router weights
(`ffn_gate_inp`, 48 layers, stored F32 — 192 MiB read a token) has a zero low half: they are exactly bfloat16 values
(the model was trained and released in bf16; the converter widened them). So were the convolution weights, `ssm_dt` and
`ssm_norm`; the norm gains are not (Qwen3-Next's norms are zero-centred and stored as 1 + w, which breaks the pattern).
Every F32 tensor of the 30B is exact bf16 too, its routers 48 MiB a token: the same copy would save it ~0.55 s. A bf16 copy of the routers on the card would be read in half the bytes — 96 MiB, about
**2.3 s a token** — and change no number anywhere, not even in the reference. Measured on the PC over 511 tokens: a router rounded to
bf16 gives perplexity 3.8811 and KL exactly 0 against the F32 router's 3.8811. (Rounded to int8 per row it is not free:
3.9094, KL 3.5e-3 — more than the int8 activations' own 2.2e-3.) Building it means a second file on the card read through the board's card pipeline (the
model file itself is never touched: its sha256 is the proof it arrived) — card-layer work to do with the board on the
bench, after the first 80B run, not before it.

Not built, deliberately: running the state's PSRAM work inside the card's waits. The read the output needs cannot
move (it depends on this token's q, k, v), and deferring the write-back costs a second read of every row — on the
single-line chips that costs more than the card time it could hide. Measure first.
