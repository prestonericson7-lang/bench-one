# psram_llm suite -- 20260928-052543-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| chat_spi_i2c | 1 | 45 | 4 | 10 | 1538.6 | 153.86 | 55.72 | 329.3 | 1203.5 | 853.4 | 5.8 | 43.8 | 49/49 | none | 0 | -- | "SPI (Serial Peripheral Interface" |
