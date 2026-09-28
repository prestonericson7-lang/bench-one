# psram_llm suite -- 20260928-022632-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| together8.france | 1 | 5 | 16 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 21/21 | none | 0 | -- | " Paris. Paris is the largest city in France and is known fo |
| together8.arith | 1 | 14 | 16 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 30/30 | none | 0 | -- | " 42\x0AB: 43\x0AC: 44\x0AD" |
| together8.fib | 1 | 4 | 16 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 20/20 | none | 0 | -- | "    if n <= 0:\x0A        return 0\x0A    elif n == " |
| together8.js | 1 | 9 | 16 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 25/25 | none | 0 | -- | " a + b;\x0Aconst subtract = (a, b) => a - b;\x0A" |
| together8.colors | 1 | 34 | 16 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 46/46 | none | 0 | -- | "The three primary colors are red, blue, and green.</im_end/ |
| together8.french | 1 | 37 | 16 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 38/38 | none | 0 | -- | "Bonjour</im_end/>" |
| together8.reverse | 1 | 43 | 16 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 59/59 | none | 0 | -- | "You can reverse a list in Python using a one-liner with the |
| together8.psram | 1 | 36 | 16 | (group) | ? | ? | ? | ? | ? | -- | ? | ? | 44/44 | none | 0 | -- | "PSRAM stands for Phase Change Memory.</im_end/>" |

## Prompts answered together (::multi)

| group | run | prompts | positions | passes | one at a time | s/pass | total s | compute s | card MB/s | max C | all = PC | same as run 1 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| together8 | 1 | 8 | 211 | 39 | 128 | 218.64 | 8527.1 | 5178.9 | 21.55 | ? | 8/8 | -- |
