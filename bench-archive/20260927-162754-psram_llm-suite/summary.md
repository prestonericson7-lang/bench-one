# psram_llm suite -- 20260927-162754-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ab_default | 1 | ? | ? | ? | ? | ? | ? | ? | ? | -- | ? | ? | 0/0 | none | 0 | -- |  |
| ab_slice200 | 1 | ? | ? | ? | ? | ? | ? | ? | ? | -- | ? | ? | 0/0 | none | 0 | -- |  |
| ab_unaligned | 1 | ? | ? | ? | ? | ? | ? | ? | ? | -- | ? | ? | 0/0 | none | 0 | -- |  |
| ab_no_overlap | 1 | ? | ? | ? | ? | ? | ? | ? | ? | -- | ? | ? | 0/0 | none | 0 | -- |  |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 45.7 |
| 1 | BENCH sd_seq read_size 4096 mb_s 7.26 |
| 1 | BENCH sd_seq read_size 16384 mb_s 7.26 |
| 1 | BENCH sd_seq read_size 65536 mb_s 7.26 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.698 mb_s 5.59 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.45 mweights_s 114.55 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.59 mweights_s 105.48 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 139.43 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.737 mweights_s 104.58 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.405 mweights_s 136.20 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.576 mweights_s 167.80 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.44 mweights_s 77.26 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 121.22 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.60 mweights_s 78.74 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 121.49 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.61 mweights_s 78.75 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 121.49 |
| 1 | BENCH psram_raw bank Y2 mode single 0x03 write_mb_s 6.44 read_mb_s 2.87 wrong 0 |
| 1 | BENCH psram_checked bank Y2 write_mb_s 2.03 read_mb_s 1.47 errors 0 |
| 1 | BENCH end temp_c 44.4 |
