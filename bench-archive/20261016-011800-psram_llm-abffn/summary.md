# The 2026-10-16 build on the board

`I bench-one psram_llm v9 banks 6 ps_bytes 50331648 max_seq 1956 gen 16 sdio dma overlap 1 align 1 slice 0 clk 66000 model qwen3b.gguf`


## Kernel (::kbench): cycles per weight per vector, fewest of 5 runs over 16 rows of ffn_gate L0

| vectors | row buffer (DTCM), production kernel | row buffer, pre-widened | OCRAM, production | OCRAM, pre-widened | bit-identical | die C |
|---|---|---|---|---|---|---|
| 2 | 3.964 | 3.949 | 3.965 | 3.950 | yes | 47.7 |
| 3 | 3.682 | 3.583 | 3.681 | 3.583 | yes | 47.0 |
| 4 | 3.485 | 3.477 | 3.484 | 3.477 | yes | 47.0 |
| 5 | 3.414 | 3.355 | 3.414 | 3.355 | yes | 47.7 |
| 8 | 3.232 | 3.220 | 3.232 | 3.220 | yes | 47.0 |

## The batched pass's gate/up read order: ::ffngroup 32 (the old order) against 256

| prompt pass | G | pass s | card MB | card wait s | card MB/s (wait only) | arithmetic s | die C |
|---|---|---|---|---|---|---|---|
| 2 positions | 32 | 131.2 | 1834.8 | 83.4 | 22.01 | 47.8 | 46.4 |
| 2 positions | 256 | 115.4 | 1834.8 | 67.5 | 27.17 | 47.8 | 48.3 |
| 5 positions | 32 | 142.2 | 1834.8 | 37.7 | 48.71 | 104.4 | 48.3 |
| 5 positions | 256 | 140.7 | 1834.8 | 36.2 | 50.74 | 104.4 | 50.3 |

| realistic chat (45-token prompt, 4 generated) | G | total s | prompt s | per pass s | card wait s | hidden s | die C max |
|---|---|---|---|---|---|---|---|
| | 32 | 1376.2 | 955.7 | 137.62 | 351.3 | 846.4 | 49.6 |
| | 256 | 1384.8 | 964.5 | 138.48 | 360.0 | 813.2 | 50.9 |

Step lines: G=32 49, G=256 49, reference 48. G=32 vs G=256: IDENTICAL. G=256 vs reference: DIFFERENT.

## Correction (same night, checked)

The line above says "G=256 vs reference: DIFFERENT" because the reference file the script was given was made
from a different prompt: it was produced through a shell `$(python -c ...)`, and on Windows Python's text-mode
stdout turned every `\n` into `\r\n` (token 319 instead of 198) and the command substitution dropped the
final newline, so it had 44 prompt tokens and 48 step lines against the board's 45 and 49. Regenerated with
`tests/tl_ref.exe` given the exact prompt as a process argument (as `suite.py` does) -- `reference_tl_ref.txt`
here -- both board runs are **IDENTICAL** to it: G=32 49 of 49 step lines, G=256 49 of 49, prompt ids the
same. The board computed the reference model exactly in every run of this A/B.
