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

## For the FPGA

This is the layout to carry over: independent memories, each owning whole layers, a checksum beside every
row, spare capacity that absorbs a failed memory, and nothing that spans two. A fabric with N RAM banks
runs N layers' attention at once with no shared bus at all; the arithmetic is the same and the exactness
proof (`verify_pc.py`) does not care what the memory is made of.
