# psram_llm suite -- 20260928-045737-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ab_fifo | 1 | 5 | 2 | 3 | 418.2 | 139.39 | 23.42 | 235.0 | 183.0 | 0.0 | 0.2 | 42.5 | 7/7 | none | 0 | -- | " Paris. Paris" |
| ab_adma_overlap | 1 | 5 | 2 | 3 | 417.9 | 139.30 | 23.52 | 234.0 | 183.7 | 181.8 | 0.2 | 44.4 | 7/7 | none | 0 | -- | " Paris. Paris" |
| ab_adma_plain | 1 | 5 | 2 | 3 | 580.6 | 193.54 | 13.85 | 397.4 | 183.0 | 0.0 | 0.2 | 43.8 | 7/7 | none | 0 | -- | " Paris. Paris" |

## Same prompt, different paths (must be identical to the digit)

| prompt | gen | test | run | steps sha1 | same as the first |
|---|---|---|---|---|---|
| `The capital of France is` | 2 | ab_fifo | 1 | c2b962454e77 | yes |
| `The capital of France is` | 2 | ab_adma_overlap | 1 | c2b962454e77 | yes |
| `The capital of France is` | 2 | ab_adma_plain | 1 | c2b962454e77 | yes |
