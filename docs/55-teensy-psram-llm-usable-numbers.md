# 55 — A 3B model on a Teensy 4.1 with 64 MB of PSRAM: the usable numbers

Measured on the board, 2026-09-27. Teensy 4.1 (Cortex-M7, 600 MHz, 1 MB of its own RAM), eight
ESP-PSRAM64H chips on a bit-banged bus (64 MB), the built-in microSD holding Qwen2.5-Coder-3B Q4_K_M
(3,085,938,688 parameters, 1.93 GB). Every figure below was printed by the board; the archive directory is
named for each. Correctness is exact: every step line the board printed equals the PC reference
(`shared/model_q.c`) to the last printed digit.

## The design (firmware v8)

| part | holds | |
|---|---|---|
| **microSD** | **the whole model**: every weight, norm and bias in `qwen3b.gguf`, read every token; the tokenizer's lookup tables in `qwen3b.tok` beside it | 1.93 GB + 8.3 MB |
| **PSRAM** | **the model's working memory only**: the attention cache, what the model has seen so far | 58.4 MB = 3,072 positions |
| **Teensy RAM** | the arithmetic: a block of weight rows at a time, the activations, the scores | 1 MB |

Nothing of the model is kept in PSRAM. `qwen3b.tok` is built by the board from the model file the first
time (PSRAM used as scratch during that first open, 13.5 s on the board, then erased), checked against the
model by a checksum at every boot, and rebuilt if it ever stops matching. The PC proof checks after every open that the PSRAM
stand-in holds no model bytes, then fills it with garbage so any tokenizer lookup still reading it would
fail (`tests/tl_host.c`).

Before v8 the PSRAM also held the tokenizer (8.3 MB) and every norm and bias (0.97 MB), and the cache had
2,048 positions. Moving them to the card cost nothing measurable and gave the cache 1,024 more positions.
On the board: the first v8 boot built `qwen3b.tok` (8,301,328 bytes, one contiguous run) in a 13.5 s open;
the second found it, verified it and opened the model in **3.4 s** (v7b rebuilt the tokenizer in PSRAM at
every boot: 6.8 s), then answered France exactly, 7 of 7 (`20260927-222806-psram_llm-suite`).

## What a user gets

| | **v9c (card path per pass)** | v9 (chips as memories) | v8b (card at 66 MHz) | v8 | v7b | archive (v9c / v9 / v8b / v8 / v7b) |
|---|---|---|---|---|---|---|
| a real chat question (45 tokens with the chat template) to its **first answer token** | **1,118.4 s (18.6 min)** | 1,589.9 s | 1,615.4 s | 1,706.4 s | 1,707.9 s | `20260928-052543` / `013821` / `20260927-234517` / `211340` / `183958` |
| each answer token after that | **105.1 s** | 105.1 s | 105.7 s | 113.0 s | 113.2 s | same |
| the answer | "SPI (Serial Peripheral Interface" — 49 of 49 steps equal to the PC, logit delta 0.0 | same | same | same | same | same |
| reading a prompt | 8 tokens per 194–196 s pass: about **149 tokens an hour** | 105 | 103 | 97 | 97 | same |
| writing an answer, one user | about **34 tokens an hour** | 34 | 34 | 32 | 32 | |
| longest conversation the cache holds | **2,608 positions, and any one chip can fail** (docs/56) | 3,072, no spare | 3,072 | 2,048 | |
| PSRAM over the run | 6.5 s, 59 chip selects, 1 write redone (chip Y4), 0 rows re-read | 34.8 s | | | |

Eight prompts at once (4 plain, 4 chat, 16 tokens each, v6): 39 passes, 211 positions (72 of them shared
openings), **2.63 h** for all eight, 8 of 8 equal to the PC; a pass feeding eight answers costs 302 s for 8
tokens against 113 s for 1: **3.0× the tokens an hour** (`20260927-122709-psram_llm-suite`).

So it is a working, exact 3-billion-parameter assistant for **batch work** — questions queued and answered
overnight — and not an interactive one.

## Where a pass goes (v9c, card at 66 MHz)

| pass | total | SD card | arithmetic (Teensy RAM) | PSRAM |
|---|---|---|---|---|
| one answer token (FIFO) | 105.1 s | 76.0 s (72%, 24.13 MB/s) | 28.8 s (27%) | 0.22 s (0.2%) |
| eight prompt tokens (ADMA2, arithmetic under the read) | 193.7–195.8 s | 1.6 s left waiting; 145.7 s hidden under the arithmetic | 192 s (99%) | 0.23–1.35 s |

A prompt pass is now the arithmetic alone; a one-token pass is the card alone plus 29 s. How the card read
became free for batched passes, and why not for single ones: docs/56.

Every token reads the whole model off the card: 1,834.8 MB (the norms and biases add 0.9 MB and 0.2 s).
The card sets the one-token speed; the M7's arithmetic sets the batched speed.

**The card's clock.** SdFat runs the card at 49.5 MHz, the SD High Speed limit. At every boot v8b now reads
8 MB in four places at 66 and 99 MHz, twice each, checks every byte against the 49.5 MHz read, and keeps the
fastest divider by measured rate: 22.9 → **24.0 MB/s** at 66 MHz, and 23.9 at 99, so 66 is kept — above it
the card, not the bus, is the ceiling. A read that fails above 49.5 MHz (every block carries a CRC16 the
controller checks) drops the clock back for good. Reading the card by DMA was measured at 7.3 MB/s in all 36
watermark/burst settings and is not used. Details of both sweeps: docs/54.

## The PSRAM as working memory

Per bank, from the boot proof that fills and reads back all 64 MB (`20260927-164610-psram_llm-suite`):

| bank | bus | read | 8 MB written in |
|---|---|---|---|
| CS0 | quad | 8.82 MB/s | 0.3 s |
| CS1 | quad | 7.31 MB/s | 0.3 s |
| Y0–Y5 (via 74LVC138A) | single-bit, `0x0B` | 2.84–2.95 MB/s | 1.3–1.8 s |

Every transfer is self-checked (reads done twice, writes read back): 1.45 MB/s read and 1.84 MB/s write on a
single-bit bank (`::bench`, 0 errors).

**The banks come and go, and the boot proof catches it.** Over the 24 hours the board has now run (die
41.9–52.2 °C throughout, fan and heatsink on; the chip's own panic point is 90 °C) the number of banks the
boot proof accepted was 8 on seven boots, 7 on five and 6 on four. Y0 has been marginal all day (dropped at
12:27 with 52 wrong bytes in 8 MB, accepted on the next four boots, dropped again at 23:21 with 94 and at
23:45 with 64, when it also qualified only in the slower `0x03` mode). Y4 qualified on every boot until
23:21 and has failed every timing setting on the three boots since. A dropped bank costs cache positions
(3,072 → 2,647 with six banks), never correctness: the model is only laid out on banks that just read back
all 8 MB right, and the running self-check has corrected 0 bytes since the qualification fix.

**The cost of a long conversation is PSRAM time, and it is linear.** Every pass reads each prompt's whole
cache back: v9 measures **+0.0049 s per position** of context per prompt (eight-slot passes 0.25, 0.56, 0.88,
1.19, 1.50 s — +0.31 s every 8 positions; one-token passes 0.24–0.26 s at positions 45–48; v8b was twice
that). At the full 2,608 positions that slope gives about +13 s a token — roughly 118 s instead of 105. That
figure is the measured slope extended, not a run. Why it halved, and why a chip can now fail without ending
the run: docs/56.

**Why the weights are not in PSRAM:** they are the model, and the model stays on the card — and 1.93 GB would
not fit in 64 MB, whose 2.8–8.8 MB/s is slower than the card's 18.5–22 MB/s anyway.

## Found and fixed on the way

- **A PSRAM bank on a cliff.** Bank Y1 measured its read edge at 6 on one boot and 11 on another (every other
  bank repeated exactly); run at the lucky 6 + 3, it failed the attention cache's writes at the same address
  on two prompts. The timing sweep now requires four kinds of data to read back (linear pattern, mostly
  zero, mostly 0xFF, pseudo-random); Y1 and Y2 then measure 11 on every boot. Details: docs/54.
- **Seeks walked the file** (1,144 s batched pass → 233.5 s) and **a stopped board left the card mid-transfer**
  (hung it until power-off). Both fixed; docs/54. The tokenizer's card file is created as one contiguous run
  for the same reason: its lookups are random reads.

## The card by DMA: tried, measured, not used

v7 reads the card by DMA while the arithmetic runs. On the board 98–99% of the arithmetic ran inside the card
waits, bit-exact — but this card's DMA path is capped at **7.26 MB/s** against FIFO's **18.5 MB/s** (same
49.5 MHz clock, every read size, cached or uncached memory), so it loses: 298.4 s a token against 112.6.
All five ways, one switch at a time: docs/54. Fixing the DMA rate would make a token about 99 s and an
eight-prompt pass about 195 s instead of 296.

## Kernels (the M7, per weight)

| | one vector | eight vectors, one unpack |
|---|---|---|
| Q4_K | 114.6 M weights/s | 139.4 per vector |
| Q6_K | 78.7 | 121.5 |

## Reproduce

`firmware/bench-one/tests/psram_llm/README.md`: build and flash through `suite.py --flash`; the chat
test above is `make_tests_realistic.py` → `tests_realistic.txt`, run with
`suite.py --tests tests_realistic.txt --runs 1 --bench 1`.
