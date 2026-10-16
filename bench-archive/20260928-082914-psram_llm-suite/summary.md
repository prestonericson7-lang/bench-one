# psram_llm suite -- 20260928-082914-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| chat_spi_i2c | 1 | 45 | 4 | 10 | 1378.2 | 137.82 | 52.31 | 350.8 | 1020.7 | 847.5 | 6.7 | 43.8 | 49/49 | none | 0 | -- | "SPI (Serial Peripheral Interface" |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 43.1 |
| 1 | BENCH sd_seq read_size 4096 mb_s 7.51 sdio dma |
| 1 | BENCH sd_seq read_size 16384 mb_s 7.50 sdio dma |
| 1 | BENCH sd_seq read_size 65536 mb_s 7.50 sdio dma |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 7.51 sdio dma clock_khz 66000 |
| 1 | BENCH sd_seq read_size 4096 mb_s 18.53 sdio fifo |
| 1 | BENCH sd_seq read_size 16384 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq read_size 65536 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 18.54 sdio fifo clock_khz 66000 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.682 mb_s 5.73 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.16 mweights_s 114.02 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.46 mweights_s 105.26 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 183.44 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.754 mweights_s 104.28 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.421 mweights_s 135.71 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.593 mweights_s 166.98 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.32 mweights_s 77.11 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 120.68 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.47 mweights_s 78.57 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 120.93 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.48 mweights_s 78.58 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 120.96 |
| 1 | BENCH psram_raw bank Y0 mode single 0x03 write_mb_s 5.04 read_mb_s 1.78 wrong 0 |
| 1 | BENCH psram_plat bank Y0 verified_write_mb_s 1.34 read_mb_s 1.81 errors 0 |
| 1 | BENCH cache_layer bank CS0 positions 1024 write_s 0.142 read_s 0.061 ms_per_position_read 0.060 rows_reread 0 bank_switches 1 |
| 1 | BENCH end temp_c 42.5 |
