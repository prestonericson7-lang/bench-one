# 54 — A 3-billion-parameter model on a Teensy

*Every number here was printed by the board or by a PC build checked against the project's reference
runtime. Board logs are in `bench-archive/`; the code is `shared/tl_core.c` and `firmware/bench-one/tests/psram_llm/`.*

## What ran

Qwen2.5-Coder-3B (Q4_K_M GGUF, **3,085,938,688 parameters**, 1,929,903,072 bytes) on a Teensy 4.1 whose own
RAM is 1 MB.

| where | holds | size |
|---|---|---|
| the Teensy's microSD slot | every weight, read once per token in 64 KB blocks of whole rows | 1,922,981,520 bytes per token |
| PSRAM, bit-banged | tokenizer: 151,936 pieces, 151,387 merges, two hash maps | 8,300,816 bytes |
| PSRAM | every RMSNorm gain and q/k/v bias | 966,656 bytes |
| PSRAM | attention cache, int8 plus a scale per head per position | 38,928,384 bytes (2,048 positions) |
| on-chip RAM | activations, one 64 KB row block, attention scores, the batch arena | v5 image: 489 KB RAM2 + 273 KB RAM1 of variables |

Past 571 M parameters the weights cannot live in PSRAM at any useful width: 48 MB holds 402 M weights even at
one bit each. So the card streams them, and the PSRAM is the machine's working memory — the part that has to
be random-access and writable.

## The first answer, 2026-09-27 02:47–03:24

`bench-archive/20260927-023701-psram_llm/serial.log`

> The capital of France is **Paris. Paris is the largest city in France and is known for its rich history,**

| | |
|---|---|
| tokens matching the PC reference | **21 of 21**, including the step-14 near-tie (reference margin 0.006, Teensy 0.009) |
| largest top-1 logit difference from the PC | 0.111 (step 0: 3.8e-6) |
| time per forward pass | **127.7 s** |
| of which: card | 97.1 s — 1,833.9 MB at **18.88 MB/s** |
| of which: M7 arithmetic | 29.6 s |
| of which: PSRAM | 0.78 s — about 1 MB read, 18.6 KB written |
| read from the card over the run | 37.61 GB |
| PSRAM self-check corrections | 0 |

The logit drift grows after step 0 because activations are quantized to int8 before every matrix: a
last-bit difference can move a value across a rounding boundary, and 36 layers carry it. Tokens hold
wherever the reference's own margin exceeds that drift. Two sources of last-bit differences, both found
after this run: newlib's `expf`/`powf`/`sinf`/`cosf` against the PC's, and — visible in this run's own
listing, `psram_llm.ino.lst` — 19 fused multiply-adds GCC placed in the dot-product and dequantize kernels
on ARM (`gguf_dot_q` 13, `gguf_dot_q4k_presum` 4, `gguf_dequant` 2), where the PC rounds twice. Both are
removed in v6 (below).

## How it is known to be the same model

`tests/tl_ref.c` runs `shared/model_q.c` (fused integer path) one token per position; `tests/tl_host.c` runs
`shared/tl_core.c` with weights read from the file every token and the "PSRAM" a separate store filled with
0xA5. Same compiler, same flags. Identical token ids **and** logits to nine digits on:

- "The capital of France is" + 16 tokens (21 positions)
- code, space runs, UTF-8, a control-token string, a contraction (34 positions)
- a 123-token prompt + 6 (128 positions, across the 64-position cache block)
- 16 MB of PSRAM instead of 48 (cache 395 positions); 8 MB refuses cleanly
- that 395-position cache filled to the last row: `def is_prime(n):\n` + 390 tokens, all 395 step lines
  identical, across seven 64-position attention blocks (2026-09-27, Qwen2 tokenizer); one token more is
  refused with `position 395 outside 0..394`
- the batched prompt path (`TL_PREFILL=1`) on all of the above, byte for byte, second-best token included
- 8 prompts answered together (`TL_MULTI`), with shared openings off and on — each prompt's lines equal to
  its own `tl_ref` run (`tests/psram_llm/verify_pc.py`)
- the Teensy's own M7 kernels, run on the PC through a C emulation of `SXTB16`/`SMLAD`/`SSUB8`
  (`-DGD_EMULATE_M7`): per token, batched and together, the same lines again

## PSRAM: qualified on the board, every boot

Discovery, then `luckfox/drive.py`'s procedure run by the Teensy on itself: quad first, then single-bit
0x03 and 0x0B, chip-select setup 0 and 16, the 21 µs refresh budget in both directions, four full-span
confirmations and six stability passes, a chip reset on every bank switch. Then **every usable byte is
proven**: all banks filled with bank-tagged data, then all read back.

Run 1 took the fastest setting that verified — the edge — and CS0 then returned a wrong tokenizer offset
("string 87272 too long") on real data with many zero bytes, which the test pattern never had. So:

- **MARGIN 3**: every bank runs three table steps slower than its edge, in both directions.
- **Self-checking transfers**: every write is read back; every read is done twice and, on disagreement, a
  third time with a byte majority. Corrections are counted and logged. Since the margin: **zero corrections**.

Measured on 2026-09-27 (run 2): CS0 quad 9.25 MB/s, CS1 quad 7.49, Y1–Y5 single-bit 2.83–3.11; Y0 unstable
on that boot (81 wrong on a stability pass) and left out; 7 banks, 56 MiB. The next boot took all 8 (64 MB).

Every boot that night qualified and proved the banks afresh — seven of them, in `bench-archive/20260927-0*`:

| bank | kept | left out | how it failed when it did |
|---|---|---|---|
| CS0 (quad, on the pads) | 7 | 0 | — (8.60–10.31 MB/s) |
| CS1 (quad, on the pads) | 7 | 0 | — (5.17–9.24 MB/s: the setting it qualifies at varies boot to boot) |
| Y2, Y3, Y5 (single-bit, behind the 74LVC138A) | 7 each | 0 | — (2.83–3.61 MB/s) |
| Y1 | 6 | 1 | one wrong byte in the 8 MB proof |
| Y4 | 6 | 1 | 77 wrong on a stability pass |
| Y0 | 2 | 5 | stability pass (81, 1 wrong) or the proof (64, 32, 1 wrong); always the slowest, 1.67–1.70 MB/s |

Banks in use per boot: 6, 7, 8, 7, 6, 7, 8. Nothing that failed qualification or the proof was ever used,
and the run that used banks (run 2) logged zero self-check corrections. Y0 is marginal on this wiring; the
design copes by leaving it out, and costs nothing for it: with 6 banks the cache still has room for 2,160
positions, above its 2,048 cap.

## The card: 18 MB/s, and one way to hang it

Sequential reads at 64 KB: 18.06–18.88 MB/s on the built-in slot (SdFat, FIFO_SDIO).

A flash that rebooted the Teensy **while it was reading the card** left the card answering CMD0 and CMD8 and
never finishing ACMD41 (SdFat error 0x17, 8 × 1 s, FIFO and DMA alike). A Teensy reset does not power the
card down — the 4.1's socket runs straight off 3.3 V — so only unplugging USB clears it. Since then
psram_llm stops at the next card-read boundary on `::stop`, and `tools/bench_run.py` sends `::stop` before
every flash.

Flashing has a host-side failure of its own: twice at 05:33 the upload stopped at "Unable find Teensy Loader"
while every PC core was running model checks — the loader application was up and listening, and nothing
reached the board. The same image flashed first time with the PC idle (`bench-archive/20260927-060317`).
`bench_run.py` now retries that one message, which comes before anything is sent to the board.

## On the board, 2026-09-27 afternoon: two card faults found and fixed, and the logits exact

**Seeks walked the file.** The first batched prompt pass on the board (v5, 12:00) took **1,144.1 s**: its
arithmetic was the projected 148.4 s, but the card delivered 1,833.9 MB in 992.9 s — 1.85 MB/s — and the
feed-forward stage alone took 1,081 s. `::bench` had the cause: a random 4 KB read cost 40.2 ms. SdFat on
FAT32 walks the cluster chain from the file's first cluster on every backward seek (`FatFile::seekSet`), and
the batched feed-forward alternates 32 gate rows with 32 up rows — 12,384 backward seeks a pass. One call
fixes it: `contiguousRange()` finds the file in one unbroken run (sectors 16,776..3,786,119 on this card) and
sets SdFat's contiguous flag, after which a seek is one addition. Random 4 KB read: **0.70 ms**. (A fragmented
file would get four forward-only read handles instead; the boot log says which.)

**"Stopped between two reads" was not idle.** Flashing after a clean `::stop` hung the card again (12:22). In
FIFO_SDIO mode SdFat leaves every read an open, paused multi-block transfer (`CMD18`, "infinite transfer" on
the Teensy 4) until the next non-sequential read or `syncDevice()` sends `CMD12`. psram_llm now closes it after
every command and before the boot window, so an idle board always has an idle card.

**The same prompt after both fixes** (v6, `bench-archive/20260927-122709-psram_llm-suite`):

| | v5, 12:00 | v6, 12:29 |
|---|---|---|
| batched pass, 5 prompt positions | 1,144.1 s | **233.5 s** |
| of which card | 992.9 s (1.85 MB/s) | 102.7 s (17.86 MB/s) |
| of which arithmetic | 148.4 s (29.7 s a position) | **128.0 s (25.6 s a position)** |
| step lines equal to the PC reference | tokens yes, logits drift from step 1 (18.2110 vs 18.2073) | **every digit of both logits** |

The 5-token prompt now costs 233.5 s where one token per pass costs 5 × 127.7 = 638 s: **2.7×**.

The whole France answer, run 1 of 3 (suite, 12:29–13:04): " Paris. Paris is the largest city in France and is
known for its rich history," — **21 of 21 tokens and a largest logit difference from the PC of 0.0**, through
the step-10 near tie (margin 0.006) — in **2,058.9 s over 17 passes**, where run 2 took 21 passes and 2,682 s.
PSRAM self-check: 0 corrections.

The seek fix sped up plain decoding too, because the one-token path also seeks backwards between matrices
every layer: **114.0 s a pass** (card 83.1 s at 22.07 MB/s, arithmetic 30.0 s) against run 2's 127.7 s (card
97.1 s). Those decode steps are exact as well — a different code path (`tl_forward`) from the batched one.

Kernel rates, same rows, `::bench` (per activation vector):

| | one vector | eight vectors, one unpack (v6) | |
|---|---|---|---|
| Q4_K (presum) | 114.6 M weights/s (5.2 cycles) | **139.4** | 1.22× |
| Q6_K | 78.7 | **121.5** | 1.54× |

Turning off fused multiply-adds cost the single kernels 0.7–1.3% (v5 115.5 → v6 114.6 on Q4_K presum).

The same France answer one token per pass (`france_tok`, run 1, 13:04–13:44): **21 of 21, largest logit
difference 0.0**, 2,395.1 s over 21 passes, 114.05 s each. Its 5 prompt positions took 569.7 s against the
batched pass's 233.5 s — **2.44×** on the board, same answer to the digit by both paths.

## v7 on the board, 2026-09-27 16:28–17:00: the overlap works, the DMA card path is 2.55× slower

**A PSRAM bank on a cliff.** The first v7 boot qualified 8 banks where v6's had 7, and both A/B prompts failed
at the same two PSRAM addresses (17,131,792 read corrected; 17,656,080 write never read back right) — bank
**Y1**, which had measured its `0x03` read edge at **6** on this boot and at **11** on v6's. Every other bank's
edges repeated exactly across boots. One 32 KB probe of the linear test pattern (`pat()`, each byte the last +
0x9D) had passed by luck five steps past the real edge; run at 6 + 3, the bank passed confirm, stability and
the 8 MB proof, then failed on the attention cache's data. Fix: a timing setting now counts as clean only when
four kinds of data read back (the linear pattern, mostly-zero bytes, mostly-0xFF bytes, pseudo-random bytes).
Next boot: Y1's `0x03` edge **11** again, and Y2's `0x03` edge **11** rather than the **8** that the
pattern-only probe had reported on both earlier boots — so v6 had been running Y2 at its real edge. All six
decoder banks now run `0x0B` at the settings v6 ran for hours with 0 corrections.

**The card by DMA, measured** (v7b `::bench`, one boot, both modes, clock 49,500 kHz in both):

| | 4 KB | 16 KB | 64 KB | 4 KB into DTCM |
|---|---|---|---|---|
| DMA_SDIO | 7.26 MB/s | 7.26 | 7.26 | 7.26 |
| FIFO_SDIO | 18.52 | 18.54 | 18.54 | 18.54 |

Flat across read sizes, so not a per-command cost; the same into uncached DTCM as into cached OCRAM, and the
same clock — the DMA path itself is capped. Random 4 KB reads are 0.70 ms either way.

**The overlap, measured** (`ab_default`: DMA, overlap, whole sectors, one row per `yield()`):

| pass | total | card wait left | arithmetic done during card reads | v6 (FIFO) |
|---|---|---|---|---|
| 5 prompt positions | 312.2 s | 183.6 s | **125.0 of 125.9 s** | 233.5 s |
| 1 position | 298.4 s | 268.7 s | **28.45 of 28.9 s** | 114.0 s |

98–99% of the arithmetic ran inside the card waits, and the step lines equal the PC's to the digit — the
mechanism works on the hardware.

The five ways on one image, round 1 (`bench-archive/20260927-164924-psram_llm-suite`, France, 2 tokens, one
switch changed at a time; all five 7 of 7 with logit delta 0.0 and the same step-line hash):

| card read | 5-position batched pass | 1-position pass | card wait | arithmetic hidden |
|---|---|---|---|---|
| **FIFO** | **226.1 s** | **112.6 s** | 97.8 / 83.1 s | — |
| DMA, overlap, whole sectors, one row per `yield()` | 312.3 s | 298.4 s | 183.6 / 268.7 s | 125.0 / 28.45 s |
| the same with 200 µs of rows per `yield()` | 317.0 s | 298.4 s | 188.4 / 268.8 s | 125.0 / 28.43 s |
| the same with unaligned reads | 324.7 s | 305.4 s | 196.0 / 275.7 s | 125.0 / 28.45 s |
| DMA, no overlap | 425.0 s | 321.3 s | 296.7 / 291.8 s | 0 |

Each v7 choice is right where it applies — whole sectors save 12.4 s a batched pass, one row per `yield()`
4.7 s, the overlap 112.7 s — and FIFO still wins by 86 s and 186 s. v7b in FIFO is also a little faster
than v6 (226.1 against 233.5 s, 112.6 against 114.0 s). It loses because the reads it hides behind are 2.55× slower. With FIFO's
rate it would be a card-bound pass (~99 s for one position against 114, and ~195 s against 302 for 8 prompts
together), so the DMA rate is the thing to fix; until then every test after the A/B runs in FIFO.

## Reading the card while computing (v7 source)

A v6 pass is card time PLUS arithmetic time (83.1 + 30.0 s), because in FIFO_SDIO mode the CPU itself copies
every word out of the card controller. v7 starts the card in DMA_SDIO: the controller writes the bytes, and
SdFat waits for it in `yieldTimeout()`, calling `yield()` — which psram_llm overrides to work the rows of the
block already read (`tl_core.c`: two 24 KB buffers, block i+1 read into one while block i is worked from the
other). Every row is still worked once, in the same order, so the arithmetic cannot change; only when it
happens moves. Four board switches move only the time, for an A/B on one image:

| switch | what it changes | default |
|---|---|---|
| `::sdio fifo\|dma` | how the card is read (v6 was FIFO) | dma |
| `::overlap 0\|1` | rows worked during the DMA wait | 1 |
| `::align 0\|1` | a block read as the whole sectors around it | 1 |
| `::slice N` | rows per `yield()`: 0 = one row, N = N µs of rows | 0 |

Two things found by reading SdFat and the Teensy core before the board saw any of it:

- **Unaligned reads cost two or three card commands in DMA mode.** A 24 KB block at an arbitrary file offset
  becomes a head sector and a tail sector through SdFat's cache plus a DMA middle. FIFO mode keeps one
  transfer open across reads, DMA mode starts a new one per call. So each PIPE buffer now has one sector
  of spare room, the core puts a block at its offset within a sector, and the board reads the whole
  sectors around it in one `CMD18` (contract in `tl_plat.h`).
- **`Serial.available()` calls `yield()`** whenever nothing has arrived (`cores/teensy4/usb_serial.c`). The
  stop poll ran inside the read with the work already armed, so up to 200 µs of rows ran BEFORE each transfer
  started, and were counted as hidden. The board now polls first, then arms the work; and the default slice
  is one row, so SdFat checks the transfer again after every row instead of leaving the card idle behind a
  200 µs slice.

**Proven on the PC** (`prove_overlap.py`, 12 runs): France per token, France batched, and 8 prompts together
(4 chat), with the rows worked all before the read, none before it, 3 before it, and a varying count —
every line identical to the reference. The PC platform spoils the spare sectors (0xA5/0x5A) on every read
and checks their alignment, so a core that kept anything there would answer differently. That check was
itself tested: the same core with the spare sector removed from each buffer produced " légère" and
"CTYPE" when rows were worked after the read, and was caught; with every row worked before the read it
could not be, which is why the proof runs all four interleavings.

## Batched prompt (v4)

`tl_prefill` pushes up to 8 prompt positions through each pass over the weights. Per pass the card cost is
the same 1.83 GB; the arithmetic is 8×. From the measured split above, an 8-token prompt drops from
8 × 127.7 = 1,022 s to about 97 + 8 × 29.6 = 334 s. That figure is **arithmetic from the measured split**,
not yet measured on the board; the overnight suite's `france` / `france_tok` A/B measures it.

## Several prompts at once (v5)

The card read is three quarters of a pass and does not grow with the number of positions a pass feeds. So
`tl_step` generalises `tl_prefill`: each of up to 8 slots in a pass carries its own token, position and
attention-cache region, and `tl_multi_pass` fills the slots from several prompts — one slot for every
unfinished prompt, the rest to prompts still reading their prompt text. Board command `::multi K`, then K
prompt lines.

**Proven on the PC**: 8 prompts of 5–27 tokens answered together, 40 tokens each — every prompt's lines
(ids, both logits to nine digits, second-best token) identical to `tl_ref.exe` run on that prompt alone,
including the four that stop early on `<|im_end|>`. 64 passes over the weights for 344 positions; one at a
time with the batched prompt they take 241.

**Projected for the board** (arithmetic from run 2's split, 97.9 s per pass + 29.6 s per position — not a
measurement): the night-2 group — 8 prompts, 4 of them chat through the full template, 16 tokens each —
is 48 passes and 283 positions, 3.6 h, against 5.8 h one after another: **1.6×** (1.84× for the first draft
of the group, whose chat prompts had no system turn). The gain is capped by the arithmetic: at 8 slots a
pass is 97 s of card and 237 s of M7. That makes the kernel, not the card, the next thing to measure, so v5's `::bench` also times
the Q4_K dot in stages (`BENCH q4k_stage`).

## Shared prompt openings (v6 source — PC-proven, not yet on the board)

Every chat prompt starts with the same 20-odd tokens: the template's system turn and `<|im_start|>user\n`.
A cache row depends only on the tokens up to its own position, so a prompt in a `::multi` group that opens
with the same tokens as an earlier one copies those rows (PSRAM to PSRAM, about 19 KB a position) instead
of computing them, and replays the earlier prompt's logit lines for those positions — exactly what it would
have computed. Board command `::share 0|1`, default on.

**Proven on the PC** (`verify_pc.py`, 8 prompts, 4 of them chat): every prompt's lines identical to its own
`tl_ref.exe` run, sharing on and off; 36 passes and 212 computed positions instead of 45 and 284. On the
board that is 9 × 97.9 s of card plus 72 × 29.6 s of arithmetic, about 50 minutes less for one such group
(arithmetic from run 2's split, not a measurement).
v6 is compiled and not flashed: the v5 board run is to be measured first, so the two are not confounded.

## One unpack for eight positions (v6 source — PC-proven, speed not yet measured)

At 8 slots a pass is 97 s of card and 237 s of M7 arithmetic, so the arithmetic now caps `::multi`. Part
of it is repeated work: `tl_step` dotted every weight row with up to 8 activation vectors, and each dot
unpacked the row's nibbles again. `gguf_dot_q4k_presum_n` and `gguf_dot_q_n` (Q6_K) unpack each group of 32
weights into 16-bit lanes once and multiply every vector against them; each vector's float combination is
the single kernel's expression, in the single kernel's order.

The board's kernels had only ever been checked on the board, because `SXTB16`/`SMLAD`/`SSUB8` are ARM
instructions. `gguf_dot.c` now has a C emulation of those three (`-DGD_EMULATE_M7`, straight from the
ARMv7-M reference), so the exact M7 code runs on a PC: `tests/dot_verify.c` built that way finds the
single-vector M7 kernels (Q4_K, its presum form, Q6_K) bit-identical to the scalar reference on 2,048 random
rows each, and the batched kernels bit-identical to the single-vector ones for 1, 2, 5 and 8 vectors on
1,024 rows each. q, k and v now take the presum kernel too — same bits as the plain dot, proven the same
way — so every Q4_K matrix in a pass goes through the batched path.

How much it saves is a board measurement: v6's `::bench` prints `BENCH kernel_x8` (per-vector rate with 8
vectors) beside the single-vector rate on the same rows.

## The same bits as the PC (v6 source — built, not yet on the board)

Run 2 matched the PC's tokens but not its logits (up to 0.111 apart). The two causes are now both in the
listings, not guessed: newlib's `expf`/`powf`/`sinf`/`cosf`, and fused multiply-adds GCC places in the
kernels on ARM (19 in run 2's image, 23 in v5's). v6 removes both:

- `gguf_dot.c` and `gguf_bits.c` carry `fp-contract=off` on ARM, as `tl_core.c` already did.
- `shared/tl_math.h` computes exp, sin/cos and the RoPE frequency from IEEE double additions,
  multiplications and divisions only, and both `model_q.c` (the PC reference) and `tl_core.c` (the Teensy)
  use it. `tests/tl_math_check.c`: equal to the correctly rounded result on 2,000,000 `exp` inputs over the
  softmax/SiLU range, all 524,288 RoPE sin/cos values for positions below 4,096, and all 64 frequencies —
  the same score as the PC's own libm, which is why the PC reference did not move: all 60 survey answers,
  3,494 step lines, byte-identical before and after.
- The v6 image's listing: no fused operation left in any model kernel, and no call to `expf`, `sinf`,
  `cosf` or `powf` anywhere.

What that should buy: the board printing the PC's logits to the last digit, so a board run is checked by
exact equality instead of a tolerance, and no near tie can go the other way. Should — the v6 board run is
what says whether it does. What it may cost: each fused multiply-add in the kernels' double arithmetic
becomes a multiply and an add, once per 32 weights per vector; v5's and v6's `BENCH kernel` lines on the
same rows measure the difference.

The shared kernel files are also compiled by three other sketches (`psram_layer`, `unpack_teensy`,
`unpack_esp32`); their copies were refreshed and all three compile (Teensy 4.1 and ESP32-S3), compile only —
`unpack_teensy/build.bat` and `unpack_esp32/build.bat` upload after compiling, unconditionally.

## Lookup decoding: measured on the PC, set aside

Prompt-lookup decoding (draft the next tokens by copying what followed the last occurrence of the current
1–3 tokens, verify the draft in one batched pass) is exact — `TL_LOOKUP=1` output was identical to the
reference on every prompt. Acceptance on four code prompts (measured before the tokenizer fix below, so their
ids are the old cut's), and the board time that implies (97.9 s/pass + 29.6 s/position; a rejected draft
token still costs its arithmetic):

| draft cap | passes (4 prompts) | drafted | accepted | projected board time vs plain |
|---|---|---|---|---|
| plain decoding | 181 | — | — | 25,357 s |
| 1 | 153 | 69 | 28 | −6.0% |
| 2 | 142 | 114 | 39 | −6.3% |
| 3 | 141 | 165 | 40 | −0.9% |
| 7 | 138 | 322 | 43 | **+16.0%** |

Rejected drafts are paid for in M7 time, and on this board the M7 time per position is a third of the card
time per pass, so drafting stops paying almost at once. Not added to the firmware; `::multi` projects to
1.6–1.84× on the groups above, on any prompts.

## The tokenizer was GPT-2's, not the model's (fixed 2026-09-27)

The file says `tokenizer.ggml.pre = qwen2`. `shared/tokenizer.c` — and `tl_core.c`, which copies it — cut text
into BPE chunks with GPT-2's rules, and matched `<|im_end|>` only where a chunk happened to start. Checked
against llama.cpp tokenizing the same file (its `llama-server.exe` ships inside the local Ollama install):

| | lines/records that differ from llama.cpp | tokens (llama.cpp) |
|---|---|---|
| this repository's docs and code, 27,055 lines, old tokenizer | **18,765** | 388,284 (old: 432,415) |
| after the Qwen2 pre-tokenizer (`shared/pretok_qwen2.h`) | 657 | |
| after BPE symbols became spans (201 pieces are ≥ 64 bytes; a 64-byte slot stopped all merging) | **0** | 388,284 |
| 20,000 random records — scripts, Unicode digits and spaces, emoji, specials, contractions, 400-char runs, CR/LF | **0** | 963,811 |

What the old cut got wrong: a punctuation run keeps its trailing newlines in Qwen2 (`):\n` is one token), any
one symbol may lead a word (`-S`, `_protocol`, `.ai`), letters and digits are Unicode classes rather than
"every byte above 0x7F is a letter", and a special token is split out wherever it appears
(`colors.<|im_end|>` had become `.<`, `|`, `im`, `_`, `end`, `|`, `>`). Decoding still round-tripped
exactly, which is why nothing looked wrong: every cut was a valid tokenization, just not the one the model
learned. The board's copy and the reference now produce the same ids as llama.cpp on all 47,055 inputs —
and the board's copy again when built with `-funsigned-char`, since `char` is unsigned on the M7 and signed on
the PC.

It mattered. `ppl.exe --fast` on its built-in passage (same text, same weights, same int8 cache):

| | tokens scored | total negative log-likelihood | perplexity per token | top-1 agreement |
|---|---|---|---|---|
| old tokenizer | 203 | 401.2 nats | 7.216 | 63.1% |
| Qwen2 tokenizer | 190 | **321.2 nats (−20%)** | 5.421 | 67.4% |

Every perplexity in docs 29, 30 and 33 was measured with the old cut; their comparisons between
configurations stand (same tokens on both sides), their absolute values do not. The float cache now scores
5.429 against the int8 cache's 5.421 — int8 is still free.

Chat prompts are now also rendered from the file's own `tokenizer.chat_template`
(`tests/psram_llm/chat_template.py`): with no system message it adds
`<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. You are a helpful assistant.<|im_end|>\n`, which
the hand-written prompts before this had left out.

## What it answers (PC reference, which the board reproduces)

`bench-archive/20260927-060624-psram_llm-survey`: 60 prompts, greedy, 40 tokens, the fixed tokenizer, chat
prompts through the file's own template. This is the model, not the board: the board computes the same
tokens (21 of 21 in run 2, and every PC proof above), so what it says can be surveyed at PC speed. Graded by
hand on the first answer given:

| kind | prompts | right | wrong | cut off by the 40-token budget |
|---|---|---|---|---|
| completing a fact ("The capital of Japan is") | 10 | 9 | 1 | 0 |
| arithmetic | 8 | 6 | 0 | 2 |
| completing code (Python, C, SQL, JS, Arduino, Rust) | 10 | 10 | 0 | 0 |
| chat (the template) | 32 | 26 | 4 | 2 |
| **all** | **60** | **51 of 56 gradable (91%)** | **5** | **4** |

Good at: code — every completion is correct and idiomatic (`is_prime`, recursion, binary search with the
overflow-safe midpoint, `pinMode(13, OUTPUT)` for the LED sketch); short factual and instruction-following
chat (437 for 23 × 19, 1024, "olleh", "Wo ist der Bahnhof?", Thursday, "OK" when asked for exactly OK,
alphabetical sorting, 91 is not prime, finding the `a - b` bug).

Wrong: "DNA stands for" → "acronym or abbreviation" (at a top-2 margin of 0.014 — a near tie); the ZIP-code
regex answer confuses ZIP codes with area codes; "PSRAM stands for Phase Change Memory" (no step of that answer has a top-2 margin under
0.14, so the board will say it too; with the old tokenizer it said "Phase Change Random Access Memory"); "Gracias
mucho"; "a kilogram of feathers is heavier than a kilogram of steel".

The Q/A-format arithmetic prompts answer correctly and then invent multiple-choice options ("B: 43 C: 44"),
which is what a base-style completion does with that format; the same questions through the chat template
answer and stop.

Long answers (`bench-archive/20260927-090924-psram_llm-survey`, 200 tokens — about 7 hours each on the
board one at a time): a `Stack` class completed with `is_empty`, `push`, `pop`, `peek`, `size`, `__str__`,
docstrings and the right `IndexError`s, all correct; a palindrome function correct to its brief (case and
spaces) whose own docstring example is wrong — `"No lemon, no melon"` keeps its comma and returns False;
and an explanation of microSD storage that is wrong where it matters ("tiny metal contacts called pins
represent the bits") and right only about 512-byte blocks. Code is where this model is strong.

Against the first survey (old tokenizer, no system turn), 13 of the 25 shared prompts changed answer
(`vs_survey1.md` in the survey folder): the primary colours went from red/blue/yellow to red/blue/green,
"reverse a list" from `xs.reverse()` to an explanation of `reversed()` and slicing, and the code prompts got
3 tokens shorter because `):\n` is one token.

## Against llama.cpp and against float

The same 60 prompts, greedy, the same token ids, through three implementations of the same file
(`three_way.md` in the survey folder):

- **fast** — `model_q.c`'s fused path: int8 activations in blocks of 32, int8 attention cache. This is exactly
  what the Teensy computes.
- **float** — `model_q.c` with the weights dequantized to float and a float cache (`TL_REF_FLOAT=1`): the
  stored model computed as exactly as this code can.
- **llama.cpp** — Ollama's `llama-server.exe` on the same blob: q8_K activations in blocks of 256, f16 cache.

| pair | answers identical over all generated tokens (up to 41) |
|---|---|
| fast (the Teensy) vs float | **51 of 60** |
| fast vs llama.cpp | 40 of 60 |
| float vs llama.cpp | 41 of 60 |

The Teensy's arithmetic agrees with the float computation of the model more often than llama.cpp does. At
every one of the 48 partings, at least one of the two sides had its runner-up within 0.213 in logit (the
other side can be surer — float preferred its "DNA" token by 0.68), and neither approximation is always the
odd one out: on "DNA stands for", "Blue" and "olleh" the fast path alone takes the other first token and
float sides with llama.cpp; on "7 × 8" and "the first five even numbers" llama.cpp alone does.

Step by step (`margin_compare.md`): at every generated position where all three still share the context,
fast's top-1/top-2 token pair, and how far that pair's logit gap moves between implementations:

| pair | median | 95th percentile | largest | positions |
|---|---|---|---|---|
| fast (the Teensy) vs float | **0.056** | 0.194 | 0.468 | 1,380 |
| fast vs llama.cpp | 0.119 | 0.372 | 0.948 | 1,429 |
| float vs llama.cpp | 0.117 | 0.372 | 1.122 | 1,380 |

The int8 path sits half as far from float as llama.cpp does, and llama.cpp is as far from float as from
the int8 path — its own quantization (activations in 256-wide int8 blocks, an f16 cache) is the larger
approximation. None of the three differences grows along the sequence (positions 0–9 through 60–199 are
flat), which is what rules out a position-dependent fault in the code both of our paths share (RoPE,
attention): that would have made fast and float agree with each other and drift away from llama.cpp as the
context grew.

Is it the int8 cache or the int8 activations? `cache_split.md`: the fast path with a float cache
(`TL_REF_KVF32=1`) against float. Of the 9 prompts where fast parted from float, 5 ("DNA", 12 × 12, the SQL,
"Blue", "olleh") were the cache — with a float cache they match float — and 4 were the activations. But the
float cache also parts from float on 5 prompts the int8 cache got right, so it matches float on 51 of 60,
exactly as the int8 cache does. On this board a float cache (73,728 bytes a position against 19,008) would
hold 670–784 positions on 7–8 banks instead of 2,048, and buy nothing measurable — the same verdict
perplexity gave.

## Test process

`firmware/bench-one/tests/psram_llm/suite.py` — see that folder's README. Three runs of every test, each
compared with the PC reference and with its own first run, component benchmarks three times, everything in
`bench-archive/<stamp>-psram_llm-suite/`. Runs go in rounds (run 1 of everything, then run 2, then run 3), so
an interrupted night still has every test once; `@multi` groups run through `::multi`; tests reached by
different paths with the same prompt are checked identical to the last digit.

`tests/psram_llm/verify_pc.py` is the PC half: tokenizer parity (board core, reference, llama.cpp), every
step line of per-token, batched-prompt and answered-together runs against the reference, and — when the
emulated builds are present — the M7 kernels and whole forward passes through them.
