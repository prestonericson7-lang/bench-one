# Speculative decoding from the context on the board (::lookup 3 1, ::gen 24)

`I bench-one psram_llm v9 banks 7 ps_bytes 58720256 max_seq 2608 gen 16 sdio dma overlap 1 align 1 slice 0 clk 66000 model qwen3b.gguf`

Each pass feeds the pending token plus up to 3 tokens guessed from the context (the tokens that followed the most recent earlier occurrence of the last 4 down to 1 tokens); the board keeps the guesses only as far as its own greedy choice agrees. Plain decoding = tokens after the first x the run's own no-guess pass (the ordinary one-position pass).

| prompt | kind | prompt tokens | step lines vs reference | passes / guessed / kept (board) | predicted (alpha_sim) | generation s | plain s | speed-up | passes by positions | die max C |
|---|---|---|---|---|---|---|---|---|---|---|
| `const add = (a, b) =>` | code | 9 | IDENTICAL (33) | 14 / 21 / 10 | 14 / 21 / 10 | 1491.6 | 2498.4 | 1.675x | 7x1, 7x4 | 51.6 |
| `I have 3 apples and eat one. How many apples do ` | chat | 45 | IDENTICAL (52) | 7 / 15 / 0 | 7 / 15 / 0 | 758.2 | 730.8 | 0.964x | 2x1, 5x4 | 52.2 |
| `If today is Monday, what day is it in three days` | chat | 42 | IDENTICAL (56) | 9 / 15 / 5 | 9 / 15 / 6 **(differs)** | 967.2 | 1461.6 | 1.511x | 4x1, 5x4 | 51.6 |
| `Summarize in one sentence: The Teensy 4.1 is a m` | chat | 73 | IDENTICAL (97) | 10 / 19 / 14 | 10 / 19 / 14 | 1087.3 | 2512.8 | 2.311x | 3x1, 1x2, 6x4 | 51.6 |

All prompts: generation 4304.3 s against 7203.6 s plain: **1.674x**. Every step line matches the reference.
Board counts do NOT all equal the PC prediction for every prompt.

## Correction: the prediction column, recomputed (same night)

This run was started at 04:00:50 and imported `alpha_sim.py` before its end-of-answer rule was corrected (about
04:05: an answer that ends with the end token -- the board stops at the position that produced it, so a guess
of that token is never counted as kept; the tokens yielded and the passes are the same either way). The
"Monday" row's 6 was the old rule. Recomputed with the current `alpha_sim.simulate(prompt, answer, 4, 1, 3, 24)`:

| answer | board passes / guessed / kept | predicted now | |
|---|---|---|---|
| `20261016-002444-psram_llm-survey/04` | 14 / 21 / 10 | 14 / 21 / 10 | equal |
| `20261016-002446-psram_llm-survey/12` | 7 / 15 / 0 | 7 / 15 / 0 | equal |
| `20261016-002448-psram_llm-survey/01` | 9 / 15 / 5 | 9 / 15 / 5 | equal |
| `20261016-002448-psram_llm-survey/06` | 10 / 19 / 14 | 10 / 19 / 14 | equal |

**The board's counts equal the PC prediction for every prompt.**
