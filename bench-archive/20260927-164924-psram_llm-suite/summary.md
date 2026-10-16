# psram_llm suite -- 20260927-164924-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ab_default | 1 | 5 | 2 | 3 | 909.1 | 303.05 | 7.63 | 721.0 | 183.6 | 181.9 | 4.5 | 47.7 | 7/7 | none | 0 | -- | " Paris. Paris" |
| ab_slice200 | 1 | 5 | 2 | 3 | 914.0 | 304.65 | 7.58 | 725.9 | 183.6 | 181.9 | 4.5 | 47.7 | 7/7 | none | 0 | -- | " Paris. Paris" |
| ab_unaligned | 1 | 5 | 2 | 3 | 935.6 | 311.87 | 7.36 | 747.5 | 183.6 | 181.9 | 4.5 | 47.7 | 7/7 | none | 0 | -- | " Paris. Paris" |
| ab_no_overlap | 1 | 5 | 2 | 3 | 1067.7 | 355.89 | 6.25 | 880.4 | 182.8 | 0.0 | 4.5 | 47.7 | 7/7 | none | 0 | -- | " Paris. Paris" |
| ab_fifo | 1 | 5 | 2 | 3 | 451.4 | 150.46 | 20.83 | 264.1 | 182.8 | 0.0 | 4.5 | 45.1 | 7/7 | none | 0 | -- | " Paris. Paris" |

## Same prompt, different paths (must be identical to the digit)

| prompt | gen | test | run | steps sha1 | same as the first |
|---|---|---|---|---|---|
| `The capital of France is` | 2 | ab_default | 1 | c2b962454e77 | yes |
| `The capital of France is` | 2 | ab_slice200 | 1 | c2b962454e77 | yes |
| `The capital of France is` | 2 | ab_unaligned | 1 | c2b962454e77 | yes |
| `The capital of France is` | 2 | ab_no_overlap | 1 | c2b962454e77 | yes |
| `The capital of France is` | 2 | ab_fifo | 1 | c2b962454e77 | yes |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 47.0 |
| 1 | BENCH sd_seq read_size 4096 mb_s 7.26 sdio dma |
| 1 | BENCH sd_seq read_size 16384 mb_s 7.26 sdio dma |
| 1 | BENCH sd_seq read_size 65536 mb_s 7.26 sdio dma |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 7.26 sdio dma clock_khz 49500 |
| 1 | BENCH sd_seq read_size 4096 mb_s 18.53 sdio fifo |
| 1 | BENCH sd_seq read_size 16384 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq read_size 65536 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 18.54 sdio fifo clock_khz 49500 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.699 mb_s 5.59 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.45 mweights_s 114.55 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.58 mweights_s 105.48 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 139.43 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.737 mweights_s 104.58 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.405 mweights_s 136.21 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.576 mweights_s 167.80 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.44 mweights_s 77.26 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 121.22 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.60 mweights_s 78.73 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 121.49 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.61 mweights_s 78.75 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 121.49 |
| 1 | BENCH psram_raw bank Y0 mode single 0x0B write_mb_s 5.01 read_mb_s 2.84 wrong 0 |
| 1 | BENCH psram_checked bank Y0 write_mb_s 1.84 read_mb_s 1.45 errors 0 |
| 1 | BENCH end temp_c 45.7 |
