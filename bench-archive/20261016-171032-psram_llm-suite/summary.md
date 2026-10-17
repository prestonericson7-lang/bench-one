# psram_llm suite -- 20261016-171032-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ring2.france | 1 | 5 | 4 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 9/9 | none | 0 | -- | " Paris. Paris is the" |
| ring2.arith | 1 | 14 | 4 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 18/18 | none | 0 | -- | " 42\x0AB" |

## Prompts answered together (::multi)

| group | run | prompts | positions | passes | one at a time | s/pass | total s | compute s | card MB/s | max C | all = PC | same as run 1 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ring2 | 1 | 2 | 27 | 7 | 11 | 82.79 | 579.5 | 534.4 | 293.66 | 5 | 2/2 | -- |
