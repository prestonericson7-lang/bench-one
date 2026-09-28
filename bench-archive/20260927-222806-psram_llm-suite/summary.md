# psram_llm suite -- 20260927-222806-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| france_reuse | 1 | 5 | 2 | 3 | 451.0 | 150.32 | 20.81 | 264.5 | 182.8 | 0.0 | 3.6 | 43.1 | 7/7 | none | 0 | -- | " Paris. Paris" |
