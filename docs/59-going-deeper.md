# 59 — Going deeper: the equations, the levers not yet pulled, and what follows

docs/58 said what the measured results mean and where they lead. This document goes underneath it:
it derives the machine's behaviour from its measured constants, checks each derivation against a
measured point, works out the levers that have not been pulled and what each is worth, and states what
new technology follows — with the same three labels on every line. **Measured**: printed by hardware on
this bench (docs/54–57 name the logs). **Arithmetic**: computed from measured numbers, shown.
**Proposal**: not built; what would prove it is stated. PC measurements are carried for shape only,
never for speed (docs/README).

---

## 1. The equations of a streaming machine

Everything below comes from five measured constants:

| constant | value | measured where |
|---|---|---|
| B — weight bytes read per token | 1,834.8 MB (1,922,981,520 B) | docs/54 |
| bw_fifo — card rate, processor busy | 24.13 MB/s | `20260927-234517` |
| bw_dma — card rate, processor free (ADMA2) | 17.3 MB/s | `20260928-045113` |
| a₁ — arithmetic per byte, one position | 15.7 ns (28.8 s ÷ B) | `20260928-052543` |
| a₈ — arithmetic per byte per position, eight positions sharing the unpack | 10.9 ns (160 s ÷ 8 ÷ B) | `20260928-101322` |

### 1.1 One token

The processor-busy read and the arithmetic add (measured in docs/43 for the bit-banged bus and again
here for the card):

    t₁ = B / bw_fifo + a₁·B = 76.0 s + 28.8 s = 104.8 s        measured: 105.1 s

The processor-free read lets the arithmetic hide, but the read itself is slower; for one position the
two effects cancel to the nanosecond (docs/56: 16 ns per byte lost against 15.7 gained), and the measured
one-token ADMA2 pass was 137.5 s against FIFO's 104.8. **For a single user the card sets the speed and
nothing on the board can change that.**

### 1.2 Positions per pass, and the ceiling of one node

With P positions in flight and the arithmetic hidden under a processor-free read:

    t_P = max( B / bw_dma , P · a₈ · B ) + what fails to hide

The read is fixed; the arithmetic grows with P. They cross at

    P* = 1 / (bw_dma · a₈) = 1 / (17.3e6 × 10.9e-9) = 5.3 positions

so from about five positions the pass is pure arithmetic — measured: the eight-position pass shows 1.9 s
of card wait over 160 s of arithmetic. Tokens an hour then tends to

    3600 / (a₈ · B) = 3600 / 20.0 s = 180 an hour                measured: 177

**That is the ceiling of one Teensy for a crowd, and it is set by the multiplier, not the card.** For one
user the ceiling is 3600 / 104.8 = 34 an hour and it is set by the card, not the multiplier.

### 1.3 The three levers, and who each one helps

| lever | one user | many users |
|---|---|---|
| faster storage | **yes**, directly (§1.1) | no — the read is already hidden |
| more positions per pass | no — one user has one position | **yes**, up to P* then flat |
| faster arithmetic | a little (28.8 s of 105) | **yes**, directly (§1.2) |
| **fewer bytes per token** | **yes** | **yes** — both terms are proportional to B |

The last row is the one the rest of this document keeps returning to. Every term in every equation is
multiplied by B. Halve the bytes and everything halves.

### 1.4 Many nodes: two ways to split, and what each buys

The pipeline measurements (docs/25, PC) settle the first: **split by layers and you get
capacity, not latency** — N boards give N× the tokens an hour and the same 105 s to any one user.
Arithmetic for nine Teensys each with its own card: 1,922,981,520 ÷ 9 = 214 MB a node a token, 8.9 s to
read and 3.2 s to compute, about 12 s a pass a node; nine requests in flight, one token every 12 s; one
request, 9 × 12 ≈ 108 s a token.

The second way is to split **inside every matrix**: each board reads its own rows of every weight matrix
from its own card, computes its share, and the boards exchange the activation. Docs/25 warned against
this, and the warning was correct *for a machine whose weights are in RAM*: the traffic cost more than the
RAM saved. Here the weights are not in RAM. The traffic is unchanged and the saving is time:

    per board:  read B/N over its own card,  compute a₁·B/N,  exchange 2 activations a layer

For N = 8, which divides this model's 16 query heads and 11,008-wide feed-forward evenly (9 does not):
read 76.0 ÷ 8 = 9.5 s; compute 28.8 ÷ 8 = 3.6 s; 72 exchanges of an 8 KB activation a token — at a 6 Mb/s
serial link, about 11 ms each sent whole, under a second in all; at 1 Mb/s, about 5 s. **Roughly 15 s a
token to one user on eight boards, six to seven times today's 105 s** — arithmetic; the link rate and
topology are unmeasured. The two ways compose: eight boards a group for latency, groups for capacity.

The activation is 8 KB. The weights a board reads to produce it are 240 MB. **The ratio is 30,000 to
one, and it is why moving only activations is cheap** (measured 437 µs a hop, docs/25) and why the
storage never has to move at all.

### 1.5 The module, and where the model belongs

A Teensy with a card is a storage device with a processor on it (docs/58 §2). Two arithmetic facts about
that module:

**Bandwidth.** One $5 card streams 24 MB/s to its own processor. A thousand of them stream 24 GB/s in
aggregate — within a factor of 1.2 of this desktop's measured DRAM read (29.4 GB/s, docs/23) — out of
flash, which costs nothing to hold. The card is $5 of the module's $50; the processor beside it is the
cost. That is docs/36's finding from the other direction: the scarce thing is addressable memory with a
processor next to it, and that is a board problem, not a purchasing one.

**Hold power.** DRAM pays to hold a bit whether or not it is read: 18.4 W a terabyte in self-refresh
(published IDD6, docs/37). Flash pays nothing. A model is written once and read forever, which is
flash's one good case (writes wear it; reads do not). **The model belongs in flash; DRAM is for the
working memory** — the rule the owner set for this board on Sept 27 ("the model stays on the card, the
RAM runs the brain") is also the right answer on power at any scale, and F1 is the proof that a machine
built that way runs.

---

## 2. Levers not yet pulled

Each with its arithmetic and the single measurement that would settle it.

### L1 — Speculative decoding from the prompt: one user, two to three times faster, exact

The read is a fixed cost per pass and positions are nearly free (§1.2). A pass can therefore **verify
several guessed tokens at once**: guess k tokens, run one pass over k + 1 positions, keep the longest
prefix the model agrees with. With greedy decoding this is exact by construction — the accepted tokens
are precisely the ones the model would have produced one at a time, so the step lines must be identical
to a normal run, which is the test.

If each guess is right with probability α, a pass yields (1 − α^(k+1)) / (1 − α) tokens on average.
Measured pass costs: 5 positions 143.0 s (`20260928-045737`), 8 positions 161.3 s (`101322`), 1 position
104.8 s.

| α (guess accuracy) | 4 guesses, 5-position pass, 143 s | 7 guesses, 8-position pass, 161 s |
|---|---|---|
| 0.3 | 1.43 tokens → 100 s a token | 1.43 → 113 s (worse than 105) |
| 0.5 | 1.94 → **74 s** | 1.99 → 81 s |
| 0.7 | 2.77 → **52 s** | 3.14 → **51 s** |
| 0.9 | 4.10 → **35 s** | 5.70 → **28 s** |

Break-even is near α = 0.3. The guesses must cost nothing, which rules out a second model read off the
card (a 0.5B draft would cost 17 s a guess); it does **not** rule out guessing from the text already in
the context — repeat the continuation that followed the last few tokens the previous time they appeared.
On code and on structured answers that is often right; on prose less so. α is a property of the model and
the text, not of the board, so **it can be measured on the PC in minutes** on the survey prompts. That
measurement is the first step; the board work after it is a rollback of rejected cache rows, which the
one-layer-per-chip layout makes a matter of overwriting rows from the accepted position.

*Label:* arithmetic on measured pass costs; α unmeasured.

### L2 — A 30-billion-parameter mixture-of-experts at the 3B's speed

Qwen3-Coder-30B-A3B runs in this project's core on the PC and agrees with llama.cpp token for token
(docs/33, measured shape). Per token it touches 8 of its 128 experts a layer. From its shape — dim 2048, 32 heads of
128, 48 layers, 128 experts with 8 active are recorded in docs/33; 4 key-value heads and a 768-wide
expert are from the published configuration — about 3.0 billion active parameters a token — attention 18.9M, experts 37.7M and router 0.3M a layer
over 48 layers, plus the 311M output head read in full — against the dense 3B's 3.09 billion. **At the
same quantisation that is the same B, within a few percent.** On a read-bound machine, same B means same
time: about 105 s a token to one user, 180 an hour to a crowd, for a model with ten times the parameters.

What changes: the file is about 18 GB (a 32 GB card); the read is no longer one sequential sweep but a
gather of eight expert blocks a layer — 384 seeks a token, each about a millisecond now that seeks are
O(1) (docs/54), so under half a second; the attention cache is 48 × 2 × 512 bytes a position against
36 × 2 × 256, 2.7× more, so about a third of the positions (roughly 950 on eight chips, arithmetic).
And what it is for: on prose the 30B is *worse* than the 3B (perplexity 7.513 against 7.216) and on code
it is better where it counts, top-1 agreement 46.9% against 41.3% (docs/33, PC). It is a better code
model, not a better model.

docs/33 called streaming experts "unusable" — at 18.5 to 36.6 s a token against a resident-memory
machine that answers in under a second. Against this board's 105 s it is the same speed for a bigger
model. **The verdict flipped because the frame did.**

*Label:* arithmetic from measured B and the model's published shape; the gather's real cost on the card
is the measurement.

### L3 — The M7's stall budget

The batched Q4_K kernel runs at 183.4 million multiply-adds a second: 600 MHz ÷ 183.4 M = **3.27
cycles a multiply-add**. Its compiled loop is 34 instructions for 32 multiply-adds, 1.06 instructions
each. So the core issues about **one instruction every three cycles** in this loop (IPC ≈ 0.33), against
the M7's one to two a cycle. Two thirds of the time is stalls — load latency and accumulator dependency
chains, docs/56's reading, unmeasured which.

If the loop reached IPC 0.8: 1.33 cycles a multiply-add, 452 M a second, the eight-position pass's
arithmetic 160 → 65 s — at which point the card (106 s by ADMA2) is the wall again and prompt reading is
8 × 3600 / 106 = **272 tokens an hour from 177**. One instrumented build answers where the cycles go:
the M7's cycle counter (DWT CYCCNT) read around each instruction group of one row. That is a day, and it
decides whether the kernel has another 1.5× in it or not.

*Label:* arithmetic; the stall breakdown is the measurement.

### L4 — Fewer bytes per token: two-bit weights, and width chosen per tensor

Two-bit weights over the bit-banged bus gave 1.96× the multiply-add rate of four-bit for the same
instructions a weight (12.84 against 6.56 MMAC/s over seven banks, docs/48, measured). On a read-bound machine the gain is more direct: **half the
weight bytes is half the read**, 76 s → 38 s for the one-token pass, before any kernel gain. What it
costs the model is unmeasured, and the yardstick to measure it exists (`quality`: perplexity float32
5.429, int8 KV 5.421 — measured on the PC, and perplexity is a model property so the PC is the right
place). docs/29 found the KV cache at 4 bits destroys the model (perplexity 47,432 against 7.216) while int8 is free; weights will have
their own curve, and it will differ by tensor. The lever is **width chosen per tensor by measured
sensitivity**, written into the file. The format already carries per-tensor types; nothing new is
needed to store the choice, only to make it.

*Label:* the rate is measured; the quality cost is the measurement to make.

### L5 — A second storage channel (docs/58 Stage 1b)

Restated for completeness: a USB drive on the Teensy's host port carrying half the model, both halves
streaming by DMA, read time B ÷ (17.3 + R) MB/s with R unmeasured; at R = 17.3, 56 s a token from 105,
the arithmetic hidden. Combined with L1 at α = 0.7 the single-user token would be near 28 s; with L4 at
two bits, near 14 s. Those compose multiplicatively because each shrinks a different factor (B, the
positions per pass, the rate) — arithmetic, and the composition is the thing to distrust until measured.

---

## 3. What follows: technology, not just this build

### I1 — A model file laid out for streaming

The pass reads the file once, front to back, and the arithmetic is scheduled to the arrival of bytes.
**The file's layout is therefore the program's schedule.** A format designed for this machine would:
keep every row's scales beside its weights (Q4_K already does; it is why it streams well); order tensors
in pass order so the sweep never seeks (GGUF happens to; a format could guarantee it); carry a checksum
per block for media without their own — the card's bus has CRC16 on every block (measured: a failed read
above 49.5 MHz shows as a failed read, never as wrong bytes), a USB drive or a DIMM through the bridge
does not; and carry per-tensor width chosen by measured sensitivity (L4). Nothing in that list is
exotic; all of it follows from reading B once per token.

### I2 — The bit-exact inference profile, and transcripts anyone can check

Two architectures already agree to the last digit (F2). The rules that make them agree fit on a page:
integer accumulation in a fixed order per format; scale products in double in a stated order; exp, sin,
cos and the RoPE frequencies from correctly-rounded double (`tl_math.h`, equal to the correctly rounded
result on 2,000,000 `exp` inputs, measured); no fused multiply-add; stated rounding points to float32.
Written down and paired with the existing conformance tests (`verify_pc.py`, `dot_verify.c`), that is a
**profile**: any implementation on any hardware either passes or does not.

What a profile buys that speed never can: an output becomes a single canonical value of (model file,
input). A run can publish a hash of every step's scores as a **transcript**; anyone with the file can
recompute any step and compare. A $30 board that takes 105 s a step cannot re-run a data centre's day —
but it can re-run any hundred steps of it chosen at random, and a forged transcript has to be right at
every one. The asymmetry favours the checker. *Label:* the agreement is measured; the profile is a
document to write; the third implementation (the Zynq fabric engine, docs/58 Stage 4) is what makes it
a profile rather than a coincidence.

### I3 — Memory from scrap: the rules, and what they do not cover

The pattern that let Y0 and Y4 fail half the boots without a wrong answer (F3) generalises into five
rules: (1) one unit of work per memory, never spanning two; (2) a checksum beside every row, written with
it; (3) spare units at least the size of the largest memory; (4) qualify every memory at every power-on
by filling *all* of them with tagged data and only then reading all back; (5) retire on the first
unresolved error and re-run from a known point.

The checksum is FNV-1a, 32 bits: a corrupted row passes it with probability about 2⁻³², 2.3 × 10⁻¹⁰
(arithmetic, uniform corruption assumed; it is not a cryptographic hash and does not need to be — the
adversary is a chip, not a person). Eight bad rows in two days, none passed.

Applied to the DDR3 bridge (docs/38–39, simulation only): a DIMM rank is eight chips, and eight data
lines see exactly one of them. **A DIMM scrapped because one chip failed its memory test has seven
working chips and one the pattern would retire** — the e-waste thesis at its most concrete: the DIMMs that
fail testing are the cheapest memory on earth and the rules above are written for them.

What the rules do *not* cover: the processor's own SRAM, registers and cache. A flipped bit there is not
caught by a row checksum. F2 covers it differently — two boards computing the same step disagree — at
twice the cost. State both when the words "radiation" or "hostile" come up; the memory pattern is half
of that answer.

### I4 — The compute-at-storage module and its wire protocol

The module exists (a Teensy, a card, eight PSRAM chips, one firmware). Its protocol exists in outline:
the pipeline stage header (docs/25: 40 bytes, a stream id, one cache per stream) carries an 8 KB
activation between stages, and the vocabulary split (docs/25) shows how the output head divides so no
module holds all of it. The thing to write is the small document that fixes the header, the activation
format (float32, dim × 4 bytes) and the two split modes (§1.4), so a module built by someone else — on a
different microcontroller, from a different card — joins the group and passes the byte-identity test.

### I5 — Archival AI: a model kept the way a book is kept

A card, a board, and three C files with no dependencies (`tl_core.c`, `gguf_dot.c`, `gguf_bits.c`) give
the same digits today that they will give in twenty years, on any hardware that meets the profile (I2).
Nothing rots: no operating system, no service, no library version. The knowledge is a file; the runtime
is a page of rules; the proof is a comparison. That is a property books have and software mostly does
not. *Label:* exists in principle today (F2, the code as it stands); the long-term claim is a claim about
time and is only ever proven by time.

---

## 4. The lost techniques, re-read under these constraints

The owner's thesis (docs/20) is that techniques from the 1950s–90s died of constraints that have since
vanished, and can be revived where they win now. Three of them are visible in this machine already:

- **Scheduling computation to a sweeping medium.** Drum-memory machines placed each instruction on the
  drum so it arrived under the head the moment the previous one finished ("optimum programming"; the IBM
  650's assembler did it automatically). Cheap random-access RAM killed the technique. This board revives
  it: the weights sweep past at 24 MB/s and the arithmetic is scheduled to their arrival; the batched pass
  is the optimisation of doing everything the current byte allows before it is gone.
- **Algorithms for sequential media.** Tape sorts, and later the external-memory model that counts cost in
  blocks transferred rather than operations. The equations of §1 are that model with B as the block count.
  Every design decision in docs/54–56 — batching, ADMA2, the layer-per-chip layout — falls out of it.
- **Checksummed storage and spare capacity.** Core and drum stores carried parity and spares because the
  parts failed. Cheap reliable DRAM made that look quaint. Cheap *unreliable* memory — scrap, hand-soldered,
  hot — brings it back (I3).

And the counterweight the thesis insists on: a Cortex-M7 does most of what those machines did in
software, faster. What survives revival is the **organisation**, not the hardware — where the bytes go
and when, not what they go through. That is the honest reading and it is enough.

---

## 5. What could change — deeper, and with its conditions

**The knowledge is a file you can mail.** A 1.9 GB card holds a 3B model; a 32 GB card holds the 30B
(L2). Distributing a model becomes distributing a physical object — like a book: no network, no account,
verifiable by hash, working in twenty years (I5). Where the network does not exist or must not be used,
that is the whole difference. *Condition:* docs/58 Stage 2 for a usable queue; none for the principle.

**Checking becomes cheaper than claiming.** With a profile and transcripts (I2), any single step of any
machine's output can be recomputed by anyone for $30 and 105 s. Claims about what a model said, or
whether a stated model produced a stated decision, become checkable facts. *Condition:* the profile
written and a third architecture passing it (Stage 4).

**Memory that was thrown away comes back.** Memtest-failed DIMMs, marginal chips, hand-soldered arrays:
the rules of I3 make them usable, and the bridge (docs/38) already reads them in simulation. *Condition:*
Stage 5 — bytes back from a DIMM on this bench.

**The energy question, honestly.** The maker publishes about 100 mA at 5 V for a Teensy 4.1 at 600 MHz
— half a watt (published, not measured here). At 105 s a token that is about 50 J a token for one user
and about 10 J in an eight-position pass; a data-centre accelerator at full load does far better a token
and this machine should never be sold on energy per token. What it has instead is structural: the model
costs nothing to hold (§1.5) and the node draws nothing when the queue is empty, which is the shape that
fits a solar panel and a battery. *Condition:* Stage 1a — measure it.

**Latency to one person can fall an order of magnitude on cheap boards.** §1.4's eight-board split
(~15 s), L1 (÷2–3), L4 (÷2), L5 (÷2) compose in arithmetic to single-digit seconds a token for one user
on under $500 of parts, exact. Each factor is a separate measurement and the composition is the thing to
distrust. *Condition:* every one of them.

**The 30B on the same board.** L2 is the single largest change available with no new hardware: a
32 GB card. *Condition:* the gather's cost measured on the card, and the cache arithmetic confirmed.

---

## 6. The order to pull the levers

Measurements first, builds second; PC where the quantity is a model property, board where it is a speed.

| # | measurement | where | settles |
|---|---|---|---|
| 1 | α: how often a guess taken from the context matches the model's next token, on the survey prompts | PC (model property) | whether L1 is 2× or nothing |
| 2 | the 30B's bytes a token at Q4_K_M, its cache bytes a position, and its expert-gather pattern | PC (file property) | L2's arithmetic |
| 3 | perplexity of the 3B with each tensor class at 2 bits, one class at a time | PC (model property) | L4's per-tensor widths |
| 4 | the board's power during a pass | board, a USB meter | every energy sentence |
| 5 | the M7 cycle counter around each instruction group of one row | board, one build | L3: is there 1.5× in the kernel |
| 6 | the USB host path's raw read rate | board, one build | L5's R |
| 7 | then, one board build per lever, each proven by identical step lines before it is timed | board | — |

Items 1–3 need no hardware and no risk to it. Items 4–6 are one instrumented build each. Nothing in this
document is real until its row here has a number.

---

## 7. What this is not

docs/58 §6 stands: not fast, not interactive, not a smart model (51 of 56 on the PC survey), and nothing
past Stage 0 is proven. Add one thing. The compositions in §5 are arithmetic on independent factors, and
independent factors have a way of turning out not to be. Report each alone, measured, before reporting
the product.
