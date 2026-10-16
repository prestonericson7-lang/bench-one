# psram_llm suite -- 20260927-234517-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| chat_spi_i2c | 1 | 45 | 4 | 10 | 2038.4 | 203.84 | 22.89 | 801.7 | 1201.9 | 0.0 | 34.8 | 43.8 | 49/49 | none | 0 | -- | "SPI (Serial Peripheral Interface" |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 43.1 |
| 1 | BENCH sd_seq read_size 4096 mb_s 7.50 sdio dma |
| 1 | BENCH sd_seq read_size 16384 mb_s 7.50 sdio dma |
| 1 | BENCH sd_seq read_size 65536 mb_s 7.50 sdio dma |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 7.51 sdio dma clock_khz 49500 |
| 1 | BENCH sd_seq read_size 4096 mb_s 18.53 sdio fifo |
| 1 | BENCH sd_seq read_size 16384 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq read_size 65536 mb_s 18.53 sdio fifo |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 18.53 sdio fifo clock_khz 49500 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.682 mb_s 5.73 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.15 mweights_s 113.99 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.42 mweights_s 105.18 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 139.35 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.758 mweights_s 104.20 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.426 mweights_s 135.57 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.598 mweights_s 166.77 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.27 mweights_s 77.04 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 121.15 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.43 mweights_s 78.52 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 121.42 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.44 mweights_s 78.53 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 121.42 |
| 1 | BENCH psram no free megabyte above the attention cache |
| 1 | BENCH end temp_c 43.1 |
