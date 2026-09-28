# 55 — A 3B model on a Teensy 4.1 with 64 MB of PSRAM: the usable numbers

Measured on the board, 2026-09-27. Teensy 4.1 (Cortex-M7, 600 MHz, 1 MB of its own RAM), eight
ESP-PSRAM64H chips on a bit-banged bus (64 MB), the built-in microSD holding Qwen2.5-Coder-3B Q4_K_M
(3,085,938,688 parameters, 1.93 GB). Firmware `tests/psram_llm` v7b, card read in FIFO mode. Every figure
below was printed by the board; the archive directory is named for each. Correctness is exact: every step
line the board printed today equals the PC reference (`shared/model_q.c`) to the last printed digit.

## What a user gets

| | measured | archive |
|---|---|---|
| a real chat question (45 tokens with the model's chat template) to its **first answer token** | **1,707.9 s (28.5 min)**, 6 passes | `20260927-183958-psram_llm-suite` |
| each answer token after that | **113.2 s** | same |
| the answer | "SPI (Serial Peripheral Interface" — 49 of 49 steps equal to the PC, logit delta 0.0 | same |
| reading a prompt | 8 tokens per 294–298 s pass: about **97 tokens an hour** | same |
| writing an answer, one user | about **32 tokens an hour** | same |
| eight prompts at once (4 plain, 4 chat), 16 tokens each | **2.63 h** for all eight (39 passes, 211 positions, 72 of them shared openings); 8 of 8 equal to the PC | `20260927-122709-psram_llm-suite` (v6) |
| the same eight one at a time | 128 passes instead of 39 | same |
| a pass feeding eight answers | 302 s for 8 tokens, against 113 s for 1: **3.0× the tokens an hour** | same |

So it is a working, exact 3-billion-parameter assistant for **batch work** — questions queued and answered
overnight — and not an interactive one.

## Where a pass goes

| pass | total | SD card | arithmetic | PSRAM |
|---|---|---|---|---|
| one answer token | 113.2 s | 83.1 s (73%, 22.06 MB/s) | 28.8 s (25%) | 1.3 s (1%) |
| eight prompt tokens | 294–298 s | 97.8 s (33%, 18.75 MB/s) | 192 s (65%) | 4.5–7.3 s (2%) |

Every token reads all 1.83 GB of weights off the card. The card sets the one-token speed; the M7's
arithmetic sets the batched speed. The PSRAM is 1–2%.

## What the 64 MB of PSRAM does

It holds everything the model needs besides the weights: the tokenizer (8.3 MB), every norm and bias
(0.97 MB), and the attention cache (38.9 MB) — **2,048 positions**, shared among the prompts answered together,
18.6 KB a position (int8 keys and values, 36 layers). 48.2 of 64 MB used.

Per bank, from the boot proof that fills and reads back all 64 MB (`20260927-164610-psram_llm-suite`):

| bank | bus | read | 8 MB written in |
|---|---|---|---|
| CS0 | quad | 8.82 MB/s | 0.3 s |
| CS1 | quad | 7.31 MB/s | 0.3 s |
| Y0–Y5 (via 74LVC138A) | single-bit, `0x0B` | 2.84–2.95 MB/s | 1.3–1.8 s |

Every transfer is self-checked (reads done twice, writes read back): 1.45 MB/s read and 1.84 MB/s write on a
single-bit bank (`::bench`, 0 errors). Corrections since the qualification fix below: **0** in every run.

**Context length costs PSRAM time, linearly.** A pass reads each prompt's whole cache back: measured
+0.011 s per position of context per prompt (one-token passes: 0.84 s at position 5, 1.28–1.31 s at 45–48;
eight-prompt passes: +0.71 s for every 8 positions). At the full 2,048 positions that slope gives about
+22 s a token — roughly 135 s instead of 113. That last figure is the measured slope extended, not a run.

**Why the weights are not in PSRAM:** 1.93 GB does not fit in 64 MB, and the PSRAM reads at 2.8–8.8 MB/s,
slower than the card's 18.5–22 MB/s.

## Found and fixed today

- **A PSRAM bank on a cliff.** Bank Y1 measured its read edge at 6 on one boot and 11 on another (every other
  bank repeated exactly); run at the lucky 6 + 3, it failed the attention cache's writes at the same address
  on two prompts. The timing sweep now requires four kinds of data to read back (linear pattern, mostly
  zero, mostly 0xFF, pseudo-random); Y1 and Y2 then measure 11 on every boot. Details: docs/54.
- **Seeks walked the file** (1,144 s batched pass → 233.5 s) and **a stopped board left the card mid-transfer**
  (hung it until power-off). Both fixed; docs/54.

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
