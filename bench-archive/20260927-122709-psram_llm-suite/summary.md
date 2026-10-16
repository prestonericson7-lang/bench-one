# psram_llm suite -- 20260927-122709-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | s/pass | card MB/s | compute s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| france | 1 | 5 | 16 | 17 | 121.11 | 21.77 | 609.1 | 17.6 | 50.9 | 21/21 | none | 0 | -- | " Paris. Paris is the largest city in France and is known fo |
| france_tok | 1 | 5 | 16 | 21 | 114.05 | 22.07 | 631.3 | 18.7 | 50.9 | 21/21 | none | 0 | -- | " Paris. Paris is the largest city in France and is known fo |
| together8.france | 1 | 5 | 16 | (group) | ? | ? | ? | ? | ? | 21/21 | none | 0 | -- | " Paris. Paris is the largest city in France and is known fo |
| together8.arith | 1 | 14 | 16 | (group) | ? | ? | ? | ? | ? | 30/30 | none | 0 | -- | " 42\x0AB: 43\x0AC: 44\x0AD" |
| together8.fib | 1 | 4 | 16 | (group) | ? | ? | ? | ? | ? | 20/20 | none | 0 | -- | "    if n <= 0:\x0A        return 0\x0A    elif n == " |
| together8.js | 1 | 9 | 16 | (group) | ? | ? | ? | ? | ? | 25/25 | none | 0 | -- | " a + b;\x0Aconst subtract = (a, b) => a - b;\x0A" |
| together8.colors | 1 | 34 | 16 | (group) | ? | ? | ? | ? | ? | 46/46 | none | 0 | -- | "The three primary colors are red, blue, and green.</im_end/ |
| together8.french | 1 | 37 | 16 | (group) | ? | ? | ? | ? | ? | 38/38 | none | 0 | -- | "Bonjour</im_end/>" |
| together8.reverse | 1 | 43 | 16 | (group) | ? | ? | ? | ? | ? | 59/59 | none | 0 | -- | "You can reverse a list in Python using a one-liner with the |
| together8.psram | 1 | 36 | 16 | (group) | ? | ? | ? | ? | ? | 44/44 | none | 0 | -- | "PSRAM stands for Phase Change Memory.</im_end/>" |

## Same prompt, different paths (must be identical to the digit)

| prompt | gen | test | run | steps sha1 | same as the first |
|---|---|---|---|---|---|
| `The capital of France is` | 16 | france | 1 | 986b8543f6be | yes |
| `The capital of France is` | 16 | france_tok | 1 | 986b8543f6be | yes |
| `The capital of France is` | 16 | together8.france | 1 | 986b8543f6be | yes |

## Prompts answered together (::multi)

| group | run | prompts | positions | passes | one at a time | s/pass | total s | compute s | card MB/s | max C | all = PC | same as run 1 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| together8 | 1 | 8 | 211 | 39 | 128 | 242.61 | 9461.7 | 5284.3 | 17.84 | 52.2 | 8/8 | -- |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 50.9 |
| 1 | BENCH sd_seq read_size 4096 mb_s 18.53 |
| 1 | BENCH sd_seq read_size 16384 mb_s 18.54 |
| 1 | BENCH sd_seq read_size 65536 mb_s 18.55 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.701 mb_s 5.57 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.45 mweights_s 114.55 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.58 mweights_s 105.48 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 139.43 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.738 mweights_s 104.57 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.406 mweights_s 136.19 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.577 mweights_s 167.74 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.44 mweights_s 77.26 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 121.22 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.59 mweights_s 78.73 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 121.49 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.61 mweights_s 78.75 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 121.49 |
| 1 | BENCH psram_raw bank Y2 mode single 0x03 write_mb_s 6.43 read_mb_s 2.87 wrong 0 |
| 1 | BENCH psram_checked bank Y2 write_mb_s 2.03 read_mb_s 1.47 errors 0 |
| 1 | BENCH end temp_c 50.3 |
