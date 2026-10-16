# 56 — One chip, one job: the PSRAM as eight independent memories

Firmware `tests/psram_llm` v9, 2026-09-28. The Teensy 4.1 carries eight ESP-PSRAM64H chips on a bit-banged
bus: two on direct chip selects (quad mode, 7–9 MB/s), six behind a 74LVC138A decoder (single-bit only,
2.8–3.1 MB/s). Each has its own timing edge, found and margined at every boot. v5–v8 hid all of that behind
one flat 64 MB address space and made the board keep up the pretence; v9 stops pretending.

## What was wrong with "one big memory"

The core laid its attention cache across the flat space as four long arrays (keys, values, key scales, value
scales), each spanning several chips. Attention on one layer therefore read keys on one chip, their scales on
another, values on a third — and every move between chips is `apply()`: a bus reconfiguration and a chip
reset with 5 µs, 5 µs, 2 ms and 50 µs waits in it. Measured on v8b: a one-token pass spent 0.91 s in PSRAM
for 854 KB of data, 0.94 MB/s against the chips' own 2.8–8.8; the difference was mostly re-selecting chips,
about eight times a layer. And a chip that failed took an arbitrary slice out of the middle of every array:
the run stopped with "PSRAM write failed", and there was nothing to do but power-cycle.

Every read was also done twice (a third on disagreement) to catch bad bytes — half the read bandwidth for
detection.

## The design

| | v8b | v9 |
|---|---|---|
| what the core sees | one 64 MB address space | N chips, each `plat_ps_bank_bytes` big, fastest first |
| a layer's cache | scattered over 3–4 chips | **one region inside one chip**: keys, key scales, values, value scales, a checksum per key row and per value row |
| chip selects | ~8 a layer, ~290 a token | once per group of layers: **7 a pass** on this board |
| read check | every read twice, majority of three | **one read + a 32-bit checksum per row** (FNV-1a over the row and its scales, written with it); a failing row is re-read up to three times |
| write check | read back, redo | unchanged |
| a chip fails mid-run | the run dies | **the chip is retired**, its layers move to spare slots on the others, the prompt re-runs from position 0; already-printed lines repeat as `R step …` for comparison |
| cache size | positions = free bytes ÷ per-position | positions chosen so every chip holds whole layers **and a spare chip's worth of layer slots stays free** (while ≥ 1,024 positions remain); otherwise the most positions that fit |

On this board: 8 chips × 8 MB, 36 layers, 1,397,888 bytes a layer at **2,608 positions** → 6 layers a chip,
48 slots, 36 used on 6 chips, **12 spare — any one chip can fail** and be absorbed. (v8 had 3,072 positions on
the same chips with no redundancy; the trade is deliberate and `TL_SPARE_MIN` sets where it flips.)

`layer -> chip: CS0 ×6, CS1 ×6, Y1 ×6, Y5 ×6, Y4 ×6, Y3 ×6`; Y0 and Y2 spare (boot of 01:40).

The tokenizer's tables are still built once in PSRAM (bank 0 as scratch — they must fit one chip, 8.30 of
8.39 MB) and then live on the card (`qwen3b.tok`, docs/55).

## Proven on the PC before the board

`tests/tl_host.c` stands in eight banks and can kill one on its n-th write (`TL_PS_FAULT=bank:n`):

- France per token, France batched, 8 prompts together: every line identical to the reference.
- **Bank 3 killed on its 200th write, per token and batched**: the core retires it ("PSRAM bank 3 retired,
  its layers moved to spare slots; re-running the prompt from position 0"), the re-run's first 5 lines are
  compared with the 5 printed before the fault — identical — and the whole output equals the reference.
- `verify_pc.py` gained section 3b for exactly this (per token, batched, and all prompts together after a
  fault). The host also checks after every open that no bank holds a byte of the model.

## Measured on the board (bench-archive/20260928-013821-psram_llm-suite)

Boot: 8 chips, card clock 66 MHz kept by the sweep, model open 3.3 s.

`::bench`:

| | v8b | v9 |
|---|---|---|
| platform read of 1 MB, single-bit chip | 1.45 MB/s (read twice) | **2.90 MB/s** (once; the checksum does the checking) |
| verified write | 1.84 | 2.01 |
| **the cache the way attention reads it**: 1,024 positions of one layer, keys and values with every checksum verified | — | **0.058 s, 0.056 ms a position, 0 rows re-read, 1 chip select** (quad chip CS0) |

The first pass of the chat test (8 prompt positions):

| | v8b | v9 |
|---|---|---|
| PSRAM time in the pass | 4.08 s | **0.25 s** |
| chip selects in the pass | (not counted; ~8 a layer by construction) | **7** (0.014 s) |
| attention stage | 5.08 s | **1.13 s** |
| whole pass | 278.7 s | 274.8 s |

The pass is the card (83.0 s) and the arithmetic (191.6 s); the PSRAM went from 1.5% of it to 0.1%.

The whole chat test (45-token prompt with the template, 4 answer tokens; the same test on v8b at the same
66 MHz card clock, `20260927-234517`):

| | v8b | v9 |
|---|---|---|
| first answer token | 1,615.4 s | **1,589.9 s** |
| each answer token after | 105.7 s | **105.1 s** |
| PSRAM time over the run (10 passes) | 34.8 s | **6.5 s** |
| chip selects over the run | — | **59** (0.12 s) |
| PSRAM time, one-token pass at position 45 | 0.91 s | **0.24 s** |
| PSRAM growth per 8 positions of context, 8-slot passes | +0.66 s | **+0.31 s** |
| rows re-read / writes redone / unresolved | 0 / 0 / 0 | 0 / **1** / 0 |
| steps equal to the PC | 49 of 49 | **49 of 49**, logit delta 0.0 |

The one redone write was on chip Y4 at 01:48:56 — one byte read back wrong and the write was done again.
Y4 is the chip that failed every timing setting on three boots the evening before and qualified again on
this one; on the flat layout that byte would have been anyone's. The context cost is now 0.0049 s per
position per prompt in an 8-slot pass (measured slope; v8b 0.010): at the full 2,608 positions the PSRAM
adds about 13 s to a token instead of 27.

Eight prompts together (4 plain, 4 chat, 16 tokens each; v6 on the flat layout at the 49.5 MHz card clock,
`20260927-122709`; v9, `20260928-022632`):

| | v6 | v9 |
|---|---|---|
| all eight answered | 9,461.7 s | **8,527.1 s** |
| a pass | 242.6 s | 218.6 s |
| PSRAM over the 39 passes | 168.0 s | **27.0 s** |
| chip selects | — | 252 (0.52 s) |
| rows caught by the checksum and re-read right | — | **7** |
| writes redone | 0 | 1 |
| prompts equal to the PC | 8 of 8 | **8 of 8** |

The 72 shared-opening positions are copied inside each layer's chip now (`kv_copy`), 6 chip selects a pass.
Seven cache rows came back wrong from a chip during the run and were caught by their checksums and re-read
right; on the flat layout with its double read those would have been caught too — the difference is the
27 s against 168.

## The card by ADMA2: the DMA path that is not capped

Simple DMA (SdFat's) reads this card at 7.3 MB/s whatever its registers say (docs/54). The same controller
has a second DMA mode, ADMA2 -- a descriptor table instead of one address register -- which is the mode
NXP's own driver uses. `::sdadma` (v9b) arms a table for SdFat's `readSectors()` and reads 2 MB of the model
at each size, every byte summed and compared with the FIFO read (`20260928-045113-psram_llm-sdadma`, reproduced by the rerun `20260928-045546-psram_llm-sdadma`, card at
66 MHz):

| read size | simple DMA | **ADMA2** | FIFO |
|---|---|---|---|
| 4 KB | | **8.31 MB/s** | |
| 16 KB | | **14.20** | |
| 64 KB | 7.56 | **17.28** | 23.94 |

All bytes right at every size. The extended sweep (`20260928-074656-psram_llm-sdadma` and `075009`, two boots) looked for
anything that moves the rate:

| ADMA2 read | descriptors | MB/s (two boots) |
|---|---|---|
| 65,024 B | one of 65,024 | 16.4 / 16.8 |
| 65,536 B | two of 32 KB | 16.5 / 17.3 |
| 65,536 B | four of 16 KB | 16.5 / 17.3 |
| 114,688 B | two of 56 KB | 17.0 / 17.1 |
| 114,688 B | four of 28 KB | 17.0 / 17.1 |
| 65,536 B, watermark 128/64/16, burst 16, enables 7 | | 16.5 / 17.3 |
| 65,536 B, watermark 128, burst 8, enables off | | 16.5 / 17.3 |

Nothing moves it: **the controller's ADMA2 path streams this card at 16.5–17.3 MB/s, full stop**, against
FIFO's 23.9 on the same bus and clock. *(Corrected 2026-10-16, below: the FIFO figure was timed with an
untimed checksum pause between pieces, in which the card read ahead; read back to back, FIFO streams at
18.5 MB/s too.)* The shape below 64 KB is a fixed cost per command (about 250 µs, the
card's set-up for a fresh CMD18, which FIFO never pays because it keeps one transfer open) on top of that
rate. So ADMA2 is 2.3× the simple-DMA path and still 30% under FIFO; what it buys is that the CPU is free
during the read.

That decides where it pays. Per byte, ADMA2 costs 1/17.3 − 1/23.9 = 16 ns more than FIFO; a one-token pass
has 28.8 s of arithmetic over 1.83 GB = 15.7 ns per byte to hide. Dead even: for one token, hiding the
arithmetic behind the slower read gains nothing, at any block size. At three positions there are 47 ns of
arithmetic per byte to hide against the same 16 ns penalty, and at eight, 105 ns -- the A/B below measures
the two ends. (My errors along the way, kept in the archive: the first sweep read 256 KB into a 64 KB buffer
and the board reset itself; the second summed past the 2 MB region for read sizes that do not divide it and
reported three sizes WRONG that were right. Both fixed; the table above is from the corrected sweep.)

**The A/B on one boot** (`20260928-045737-psram_llm-suite`; France, a 5-position batched pass then two
one-token passes; every arm 7 of 7 equal to the PC, logit delta 0.0):

| pass | FIFO | **ADMA2, arithmetic hidden under the read** | ADMA2, no overlap |
|---|---|---|---|
| 5 prompt positions | 208.6 s (card 83.0, arithmetic 125.5) | **143.0 s** (card wait left 17.0, arithmetic 125.8) | 260.9 s (card 135.2) |
| one token | **104.8 s** (card 76.0, arithmetic 28.7) | 137.5 s (card 108.5) | 159.9 s (card 131.1) |

With the arithmetic hidden, a batched pass becomes arithmetic-bound: 118 of the 135 s of card time vanished
under the 126 s of computing, **31% off the pass**. A one-token pass has only 29 s of arithmetic to hide 131 s
of card behind, so there FIFO's 24 MB/s wins. The rule that follows, and that v9c applies before every pass:
**three or more positions in the pass, ADMA2 with overlap; fewer, FIFO** (the mode switch is a card restart,
about two seconds, once per prompt).

**v9c, the rule live** (`20260928-052543-psram_llm-suite`; the chat test, 45-token prompt, 4 answer tokens,
49 of 49 equal to the PC):

| | v8b (FIFO only) | v9 (FIFO only) | **v9c (per-pass rule)** |
|---|---|---|---|
| an 8-position prompt pass | 278.7 s | 274.8 s | **193.7 s** (card wait left 1.6 s; 145.7 s of card under the arithmetic) |
| the 5-position tail pass | 213.6 s | 210.2 s | **144.5 s** |
| first answer token | 1,615.4 s | 1,589.9 s | **1,118.4 s (18.6 min)** |
| each answer token after (FIFO) | 105.7 s | 105.1 s | 105.1 s |
| the whole test | 2,038.4 s | 2,010.2 s | **1,538.6 s** |
| card time hidden under arithmetic | 0 | 0 | 853.4 s |
| reading a prompt | 103 tokens an hour | 105 | **149 tokens an hour** |

One card-mode switch per prompt (before the first pass, and back to FIFO before the first single token), 50
chip selects, 0 PSRAM corrections. A prompt pass is now arithmetic-bound: 192 s of M7 with the card
invisible behind it. The one-token pass is card-bound at 76 s of FIFO and stays there until the card itself
is faster or the DMA path's 274 µs per command is paid less often (larger pipeline blocks, or one open
transfer for a whole matrix -- the next thing to try).

Eight prompts together under the rule (`20260928-055523-psram_llm-suite`):

| | v6 (flat layout, 49.5 MHz) | v9 (chips as memories) | **v9c (+ card path per pass)** |
|---|---|---|---|
| all eight answered | 9,461.7 s | 8,527.1 s | **6,530.5 s (1.81 h)** |
| an 8-slot pass | 302 s | 274.6 s | **193.5 s** |
| card time hidden under arithmetic | 0 | 0 | 3,826.6 s of 5,146 |
| card-mode switches | — | — | 2 (to ADMA2 at the start; back to FIFO when 2 prompts were left) |
| PSRAM over the run | 168.0 s | 27.0 s | 24.3 s |
| rows caught by checksum and re-read | — | 7 | 1 |
| prompts equal to the PC | 8 of 8 | 8 of 8 | **8 of 8** |

Yesterday's estimate for eight users was 4.8 h a round; the same group is now under two hours.

## The batched kernels, two vectors at a time

With the card hidden, an eight-position pass is 192 s of arithmetic, and that arithmetic ran at 4.3 cycles
per multiply-add against the SMLAD instruction's 0.5. The compiled inner loop of the batched Q4_K kernel
said why: 24 instructions for four SMLADs, because it kept a group's 32 lane words and every vector's four
accumulators alive at once, the compiler put them all on the stack (56 stack accesses in the function), and
it re-widened the activation bytes for every row.

v9d restructures both batched kernels (Q4_K with presums; Q6_K, which carries the down projection, the
values and the whole output head -- about 40% of the multiply-adds): each weight word's lane words are made
where they are used and feed two vectors whose accumulators stay in registers. The integer sums are the same
integers in another order, which is exact; each vector's double accumulation is the single kernel's
expression, unchanged (for Q4_K the per-sub-block products `(double)d * sc` and `(double)dmin * m`, shared
by every vector, are computed once -- the same first product the single kernel evaluates). `dot_verify`
proves both bit-identical to the single-vector kernels for 1, 2, 5 and 8 vectors on 1,024 rows, on the host
and on the M7 code emulated. The Q4_K loop is now 34 instructions for sixteen SMLADs.

On the board (`::bench`, the same rows, eight vectors, aggregate multiply-adds per second):

| kernel | v9 | v9d | v9e |
|---|---|---|---|
| Q4_K presum, eight vectors | 139.4 M/s | **183.4 M/s** (+32%) | 183.4 |
| Q6_K, eight vectors | 121.5 M/s | 114.5 M/s (worse: the lane words were rebuilt per pair; a Q6_K lane word costs two source words, a shift, a mask, an OR and an SSUB8) | 120.7–121.0 |

v9e keeps Q6_K's shared lane build per offset (16 words) and puts only the two-vector accumulator loop under
it: level with the old kernel, not ahead of it. Q6_K's cost is not in its integer loop but in its double
tail -- one scale per 16 weights against Q4_K's per 32, and the vector's own scale is the first factor, so
none of it can be shared across vectors without changing the expression. At 4.96 cycles a multiply-add,
about 1.5 of them are that tail. It stays. The chat test on v9d (`20260928-080231`; a boot on which only 5 of the 8 chips qualified -- CS1 out for the
first time -- and the layout took 9 layers a chip, 1,738 positions):

| | v9c | v9d | **v9e** (`20260928-082914`, 7 chips) |
|---|---|---|---|
| an 8-position pass | 193.7 s | 164.3 s (FFN 152 → 126 s) | **161.7–163.9 s** |
| the 5-position tail pass | 144.5 s | 143.9 s | 143.8 s (card-bound: 37 s of card wait left over 105 s of arithmetic) |
| first answer token | 1,118.4 s | 970.9 s | **957.9 s (16.0 min)** |
| each answer token after | 105.1 s | 105.1 s | 105.1 s |
| the whole test | 1,538.6 s | 1,391.2 s | **1,378.2 s** |
| steps equal to the PC | 49 of 49 | 49 of 49 | **49 of 49** |

**A negative result, kept.** The one-token kernels widen the activation bytes into lane pairs for every
weight word of every row, although the vector is the same for every row. v9f makes the lane pairs once per
matrix (`gguf_widen_act`) and has the kernels load them (`gguf_dot_q4k_presum_w`, `gguf_dot_q6k_w`): two
loads per four weights instead of two SXTB16, bit-identical (`dot_verify`, both PC builds). On the board
(`20260928-090356`) a one-token pass computed for **28.7 s, exactly as before** (qkv 6.13, wo 4.63 s, unchanged
to the hundredth): the loop is bound by load latency and the SMLAD dependency chains, not by its instruction
count, so trading ALU instructions for loads moves nothing. Measured directly (`::bench`, `20260928-100627`,
`path widened` beside the originals): Q4_K presum 114.2 → 112.1 M/s, Q6_K on 11,008-wide rows 77.0 → 76.3,
on 2,048-wide rows 78.5 → 81.2 -- a wash within ±3%. The one-token path went back to its original kernels;
the widened ones stay in `gguf_dot.c` with their `dot_verify` check and bench line, so the idea is not
tried twice.

Where that leaves the two kinds of pass on this board: an eight-position pass is arithmetic-bound at about
160 s with the card's ~150 s of reading hidden under it; a one-token pass is card-bound at FIFO's 76 s plus
29 s of arithmetic that cannot hide behind a slower DMA read. Both walls are the card interface: FIFO cannot
free the CPU, and the CPU-free path streams 30% slower. *(Both halves corrected 2026-10-16, below: an
interrupt frees the CPU under a FIFO read, and the one-token pass was already at the card's own 18.5 MB/s.)* The lever past both is a second storage channel
read by DMA -- the Teensy 4.1's USB host port (480 Mb/s, EHCI with its own DMA) with half the weights on a
USB drive would run beside the card and under the arithmetic; that is hardware the bench does not have
yet, so it is a proposal with the arithmetic, not a measurement.

## 2026-10-16: the batched pass's read order, and what a card command costs

The board was re-measured first (`20261016-001951-psram_llm-state`, `002459-bench`): card 24.00 MB/s against
24.04 on 09-28, the batched kernel 183.47 against 183.44 Mw/s, an 8-position pass 161.3 s both days. Six
chips qualify (CS0, CS1 quad; Y5, Y1, Y3, Y2 single); the owner counts seven fitted.

**The cost of a pass over P positions** had been measured only at 1, 5 and 8. `pass_cost.py`
(`20261016-002736-psram_llm-passcost`; die temperature beside each):

| positions | 1 (decode) | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| seconds | 104.8 | 131.1 | 141.9 | 142.1 | 142.3 | 142.5 | 147.0 | 161.3 |
| die °C | 41.2 | 41.9 | 43.8 | 43.8 | 43.8 | 43.8 | 44.4 | 45.1 |

Three to six positions cost the same 142 s: that is the ADMA2 read itself, with the arithmetic hidden under
it. Two positions (FIFO, nothing hidden) cost 26 s more than one, and its card wait was 83.4 s against a
decode pass's 76.1 s for the same 1,834.8 MB.

**Why: the read order.** The batched feed-forward read gate and up in alternating 16-row halves -- 1,376 reads
and 688 jumps between the two tensors a layer, where the decode path reads 1,048 with one jump. In FIFO mode
every jump closes the card's open transfer and starts another; 688 × 36 layers ≈ 24,800 restarts at ≈ 0.3 ms
is 7.4 s, the excess measured. The core now computes gate and up a group of G rows at a time (default 256,
`::ffngroup`), the group cut into near-equal whole row-buffer reads, and SwiGLU is applied in place as each
up row arrives (the same expression on the same two floats, so the same bits; 8 KB less OCRAM). Proven on the
PC at G = 32, 128 and 256 against `tl_ref` (`verify_pc.py` stage 2c). On the board (`20261016-011800-psram_llm-abffn`;
step lines of every run identical to `tl_ref` on the board's exact prompt -- see that summary's correction):

| pass | G = 32 (the old order) | G = 256 |
|---|---|---|
| 2 positions (FIFO) | 131.2 s, card wait 83.4 s | **115.4 s**, card wait 67.5 s |
| 5 positions (ADMA2) | 142.2 s | 140.7 s |
| 8 positions (ADMA2) | 161.4–163.4 s, card wait 1.9 s | 163.9–165.5 s, card wait 3.9 s |

The 8-position regression was the group's tail: 256 rows at 21 a read is twelve full reads and one of four
rows, and under ADMA2 each read hides behind the previous block's arithmetic, so the read after a 4-row tail
hid behind four rows. The near-equal cut (thirteen reads of about 20 rows, the same number of commands,
reads under 8 KB down from 3,312 to 216 a pass by the PC count) is in the next build.

The 2-position pass now waits **less** on the card than a decode pass (67.5 s against 76.1 s) for the same
bytes, with the arithmetic (47.8 s) matching the kernel bench. FIFO parks the card at a block gap while the
CPU computes, and a pause per block that is longer (21 rows at 27 µs, against 19.6 µs at one position) went
with faster reads: the card appears to read ahead while parked. `::sdfifo` measures it directly.

**What the FIFO read can carry** (`::sdfifo`, `20261016-031155-psram_llm`; 2 MB per setting, every byte summed
against the file layer's read, all right). SdFat's `readData()` asks for one block (Continue Request, with
Stop At Block Gap set) and spins until it is in; here the same registers are driven by hand with a timed
slug of work between the request and the drain:

| work between request and drain | 0 | 5 µs | 10 µs | 15 µs | 20 µs | 30 µs | 60 µs |
|---|---|---|---|---|---|---|---|
| MB/s | 23.79 | 23.77 | 23.79 | 23.79 | 20.21 | 14.52 | 7.85 |
| wait left per block | 21.1 µs | 16.1 | 11.0 | 6.0 | 4.8 | 4.7 | 4.7 |

Up to 15 µs of arithmetic a block hides completely; past about 17 the card parks (the controller gates its
clock, `SDOFF`, rather than overrun) and a block costs the work plus 4.74 µs -- the CPU's own drain of 128
words from the controller's port, 37 ns a word. Without Stop At Block Gap (one unbroken CMD18) the numbers
are the same to 1%: the handshake costs nothing.

**The card reads ahead while parked.** 48 blocks back to back, then a pause, the reads alone timed:

| pause per 48 blocks | 0 | 100 µs | 200 µs | 400 µs and up |
|---|---|---|---|---|
| read time per block | 21.52 µs | 19.56 | 17.49 | **17.01** (30.1 MB/s) |

The bus delivers a block the card already holds in 17.0 µs; what the card holds depends on how long it was
left alone.

**An interrupt instead of the spin** (`irq`): read watermark 128 words, Buffer Read Ready drains a block into a
ring, the stop at the block gap asks for the next, and the main loop works whole "rows" meanwhile. 18.55 MB/s
flat from 0 to 20 µs of work a block, every byte right, 2 interrupts a block, 27.6 µs a block. A lower read
watermark (64, 32, 16 words; 4 to 16 interrupts a block, `20261016-034757-psram_llm`) changed nothing: 27.7 µs.

**And then the 21.5 µs turned out not to be the card.** `::sdtrace` (`20261016-035248-psram_llm`) puts the
cycle counter on each step of 64 consecutive blocks with nothing in between: the **polled** read also takes
27.98 µs a block (13.6 µs from the request to the first words, then the rest at the bus's rate), the
interrupt-fed one 27.8 in all three forms tried (as `::fifoirq` does it; the request held until Read Transfer
Active clears; every status bit cleared as `readData()` does). Every measurement that showed 21.5 µs had an
untimed pause in it: the 64 KB checksum between pieces, about 0.77 ms at seven cycles a byte, during which the
card read ahead -- 128 × 27.7 µs = 128 × 21.5 µs + 0.79 ms. So:

- **This card streams at 18.5 MB/s (27.7 µs a block), FIFO or ADMA2 alike.** The 09-28 comparison "ADMA2
  17.3 against FIFO 23.9" timed FIFO with read-ahead pauses in it and ADMA2 without (each ADMA2 read is a fresh
  CMD18, which the card cannot read ahead for). The table at the top of the ADMA2 section stands as measured;
  its reading -- that FIFO is the faster path at the card -- does not.
- **A one-token FIFO pass was already bound by the card**, not by card plus arithmetic: 3.757 million blocks ×
  27.7 µs = 104.1 s, against 104.8 s measured. Its 28.7 s of arithmetic ran while the card read ahead; "card
  wait 76 s + compute 29 s" was bus time plus pause time, with the card busy throughout.
- **The floor for a pass on this card is about 104 s**, whatever the arithmetic does. What lowers it is
  fewer bytes, a faster card, or a second channel -- not a cleverer read.

**The interrupt-fed read in real passes** (`::fifoirq 1` with every pass of 1–7 positions on FIFO,
`20261016-031635-psram_llm-passcost`; the 8-position pass by ADMA2 for reference):

| positions | 1 (decode) | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| this morning (FIFO, or ADMA2 from 3; `002736`), s | 104.8 | 131.1 | 141.9 | 142.1 | 142.3 | 142.5 | 147.0 | 161.3 |
| after the read order (G = 256, `011800`), s | | 115.4 | | | 140.7 | | | 163.9 |
| **`::fifoirq`** (and near-equal group reads), s | 104.0 | **105.2** | **105.4** | **108.1** | **124.6** | 142.2 | 160.5 | 161.4 (ADMA2) |
| arithmetic hidden under the read, s | 34.2 | 57.2 | 79.4 | 95.1 | 92.2 | 85.0 | 90.1 | 144.4 |

Every pass up to four positions now costs what a one-token pass costs: the card's floor, with the arithmetic
under it. From five the interrupt's drain (≈ 6 µs of CPU a block, about 24 s a pass) makes the pass
arithmetic-bound, and at seven ADMA2 is cheaper again, so the rule for this build is `::fifoirq 1` with
`::admamin 7`. The 8-position pass is back to 161.4 s: the near-equal group reads removed the regression.

**Which card sets that floor.** The final build prints the card's CID register at boot
(`20261016-061724-psram_llm`): manufacturer 0x05, OEM bytes unprintable, product name "asdfg", revision 2.2,
made 2025-10, 7,864,320 sectors (4 GB) -- placeholder fields, a no-name card. The bus delivers 30.1 MB/s when
the card has the data ready; this card's memory sustains 18.5. A card that sustains the bus's rate would
take a pass from about 104 s to about 64 s (3.757 million blocks × 17.0 µs) with nothing else changed. That is
a few dollars and two minutes of `::sdtrace`, not a measurement made here. *(Measured the same afternoon,
below: a SanDisk Ultra runs the bus at 99 MHz, and with the card streaming into a ring a token takes 42.7 s.)*

**What one ADMA2 command costs a pass.** `tl_host.exe` with `TL_READCENSUS=1` counts every read exactly as the
board's card sees it (a count of the code's reads, never a timing): a decode pass is 87,479 pipeline commands
averaging 22.5 KB (1,967 MB with sector rounding) and 182 plain reads; a 5-position pass 99,215 commands with
G = 32 and 89,927 with G = 256. With 09-28's fit (274 µs a command + 18.6 MB/s) a 5-position pass's read is
24.6 s of command set-up and 100.9 s of streaming, 125.5 s, against ≈ 141 s measured: the set-up explains
two thirds of the gap above pure streaming. (That FIFO streams faster, 41.8 µs a KB against ADMA2's 52.4, was
the reading at this point; it was wrong -- see below: both stream at about 54 µs a KB, 27.7 a block.)

**The kernel's last cycles** (`::kbench`, same archive; cycles per weight per vector, fewest of five runs over
16 real rows; every result bit-identical to the production kernel):

| vectors | 2 | 3 | 4 | 5 | 8 |
|---|---|---|---|---|---|
| production kernel, rows in DTCM | 3.964 | 3.682 | 3.485 | 3.414 | 3.232 |
| activations pre-widened | 3.949 | 3.583 | 3.477 | 3.355 | 3.220 |
| production kernel, rows in OCRAM | 3.965 | 3.681 | 3.484 | 3.414 | 3.232 |

Pre-widening the activation bytes buys 0.4–2.7%, and where the row sits buys nothing: the loop is bound by
its loads and SMLAD chains, as the one-token kernel was (above). The compiled inner loop is 36 instructions
for eight SMLADs; at eight vectors the bench's 3.23 cycles a weight is 185.6 Mw/s per vector, and `::bench`'s
real-row figure, 183.5 Mw/s (3.27 cycles), is what the passes show.

## 2026-10-16, afternoon: a real card, and the card streaming into a ring

**The card.** The owner swapped the no-name 4 GB card for a SanDisk Ultra 64 GB (CID manufacturer 0x03,
product "SK64G", exFAT; `qwen3b.gguf` copied and checked by SHA-256 against the model's own name). First boot
halted building the tokenizer store: SdFat's `ExFatFile::seekSet` refuses a position past the file's valid
length, and the core writes the store's rows from offset 512 before its header -- FAT32 allows that seek,
exFAT does not. `plat_tok_write` now fills the gap with zeros first (`20261016-101935`). The clock sweep kept
99 MHz (43.5 MB/s by its measure, against 23.9 for the old card), and `::sdtrace` on the new card: **the first
words of a block 1.65 µs after the request** (the old card: 13.6), **11.56 µs a block polled** -- the 99 MHz bus
itself. France: every logged step line identical.

**Then the CPU was the bottleneck, twice over.**

| one-token pass | |
|---|---|
| interrupt-fed FIFO (`::fifoirq 1`) | **58.7 s** -- 15.3 µs a block: the block in (10.65 µs), then the CPU drains 128 words from the port (4.33 µs, 37 ns a word) before the next can be asked for; a lower watermark only adds interrupts |
| ADMA2, one command per 24 KB read | **109.6 s** -- ~0.75 ms of card command latency on each of 87,479 reads, an A1 card's 1,500 random reads a second. (`::sdadma` said 39.6 MB/s at 64 KB: its reads had untimed checksum pauses between them again.) |

**One open DMA transfer.** NXP's own driver continues a DMA transfer from a block gap by loading a descriptor
table and setting Continue (mcux-sdk `fsl_usdhc.c`, "boot data continuous"). Three experiments on the board
settled how the controller behaves (`::sdfifo adma`, `20261016-105736`, `110127`):

- with Stop At Block Gap set, ADMA2 **pauses and continues at every block gap in the middle of a descriptor**:
  48 blocks, 48 interrupts, every byte right;
- an **End descriptor ends the data phase** itself (afterwards no transfer is active; Continue resumes nothing);
- the engine **fetches its next descriptor the moment the current one finishes** -- a table written at the
  read's last block gap is ignored (it had already parked on the following descriptor, `0x200166CC`).

So the engine is never given a new table: **one Tran over a ring and a Link back to it**, forever, and an
interrupt at each block's stop grants the next block only while the ring has room. The card streams ahead
into the ring while the core computes; a pipeline read copies its sectors out (0.51 µs a block from DTCM).
`::sdfifo ring` (`20261016-110520`, `113011`): **45.2–45.6 MB/s, one interrupt a block, every byte right**, an
8 KB ring as good as 32 KB.

**Getting it into the pass took two crashes, kept here.** In a pass the stream is closed for every norm or bias
read and reopened elsewhere, which the ring test never did. Closing through SdFat's `readStop` (it clears Stop
At Block Gap, then CMD12) and then leaving ADMA2 let the controller move data by *simple* DMA to `DS_ADDR`:
the first time SdFat had last pointed it at a buffer on the stack (crash report: instruction access violation
at `0xBF370000` -- card bytes as a return address); the second time at the scratch block it was moved to, and
the variables after it came back holding card bytes (the symbol map put `fqi_err`, `rq_prod`, `fqi_done` right
after it). The ring now has its own close -- park at a block gap, CMD12 while still in ADMA2, wait for the data
lines, reset the data side (RSTD), clear every transfer bit and status -- and `DS_ADDR` points at a 4 KB
canary while a DMA stream is open. `::sdfifo cycle` runs the production functions the way a pass does (24
runs of 8 sequential 24 KB reads, a file-layer read between runs): **384 reads, 0 wrong, 0 failed, canary
untouched** (`20261016-113914`).

**The ring in real passes** (`::fifoirq 3`, `20261016-114129-psram_llm-passcost`; every pass's step lines
identical to `tl_ref`, 8 of 8):

| positions | 1 (decode) | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| this morning, the old card | 104.8 | 131.1 | 141.9 | 142.1 | 142.3 | 142.5 | 147.0 | 161.3 |
| SanDisk, interrupt-fed FIFO | 58.7 | | | | | | | |
| **SanDisk, the ring** | **42.7** | **54.5** | **72.5** | **91.0** | **109.4** | **127.1** | **145.6** | 160.8 (ADMA2) |

**A token in 42.7 s, 2.45× this morning**, with 29.3 of its 29.8 s of arithmetic under a card wait of 12.9 s --
within four seconds of the bus's own floor (3.757 million blocks × 10.3 µs ≈ 39 s). Every pass from two
positions is now arithmetic-bound, about 18 s a position, so the M7's kernels are the next wall, not the
card. The ring is the default (`::fifoirq 3`, `::admamin 8`).

## For the FPGA

This is the layout to carry over: independent memories, each owning whole layers, a checksum beside every
row, spare capacity that absorbs a failed memory, and nothing that spans two. A fabric with N RAM banks
runs N layers' attention at once with no shared bus at all; the arithmetic is the same and the exactness
proof (`verify_pc.py`) does not care what the memory is made of.
