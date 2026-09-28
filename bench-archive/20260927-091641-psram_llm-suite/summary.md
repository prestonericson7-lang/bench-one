# psram_llm suite -- 20260927-091641-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | s/pass | card MB/s | compute s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 43.1 |
| 1 | BENCH sd_seq read_size 4096 mb_s 17.49 |
| 1 | BENCH sd_seq read_size 16384 mb_s 17.49 |
| 1 | BENCH sd_seq read_size 65536 mb_s 17.49 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 40.199 mb_s 0.10 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.96 mweights_s 115.51 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 57.35 mweights_s 106.91 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.704 mweights_s 105.19 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.406 mweights_s 136.19 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.576 mweights_s 167.79 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.91 mweights_s 77.86 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 62.01 mweights_s 79.27 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 62.03 mweights_s 79.28 |
| 1 | BENCH psram_raw bank Y0 mode single 0x03 write_mb_s 4.72 read_mb_s 1.70 wrong 0 |
| 1 | BENCH psram_checked bank Y0 write_mb_s 1.27 read_mb_s 0.86 errors 0 |
| 1 | BENCH end temp_c 43.8 |
