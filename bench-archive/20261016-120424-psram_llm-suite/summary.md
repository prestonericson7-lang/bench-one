# psram_llm suite -- 20261016-120424-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| chat_spi_i2c | 1 | 45 | 4 | 10 | 1093.1 | 109.31 | 293.26 | 62.6 | 1023.9 | 640.0 | 6.6 | 53.5 | 49/49 | none | 0 | -- | "SPI (Serial Peripheral Interface" |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 50.3 |
| 1 | BENCH sd_seq read_size 4096 mb_s 13.03 sdio dma |
| 1 | BENCH sd_seq read_size 16384 mb_s 28.62 sdio dma |
| 1 | BENCH sd_seq read_size 65536 mb_s 40.70 sdio dma |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 13.09 sdio dma clock_khz 99000 |
| 1 | BENCH sd_seq read_size 4096 mb_s 43.70 sdio fifo |
| 1 | BENCH sd_seq read_size 16384 mb_s 43.89 sdio fifo |
| 1 | BENCH sd_seq read_size 65536 mb_s 43.95 sdio fifo |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 43.66 sdio fifo clock_khz 99000 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.319 mb_s 12.24 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.19 mweights_s 114.07 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.47 mweights_s 105.28 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path widened rows 56 cols 2048 mb_s 60.00 mweights_s 111.85 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 183.49 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.758 mweights_s 104.20 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.427 mweights_s 135.53 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.597 mweights_s 166.78 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.33 mweights_s 77.12 |
| 1 | BENCH kernel ffn_down_L0 type 14 path widened rows 7 cols 11008 mb_s 59.72 mweights_s 76.34 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 121.49 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.48 mweights_s 78.59 |
| 1 | BENCH kernel attn_v_L0 type 14 path widened rows 39 cols 2048 mb_s 63.49 mweights_s 81.15 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 121.72 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.49 mweights_s 78.60 |
| 1 | BENCH kernel token_embd type 14 path widened rows 39 cols 2048 mb_s 63.52 mweights_s 81.20 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 121.74 |
| 1 | BENCH psram_raw bank Y2 mode single 0x0B write_mb_s 6.51 read_mb_s 2.83 wrong 0 |
| 1 | BENCH psram_plat bank Y2 verified_write_mb_s 2.01 read_mb_s 2.90 errors 0 |
| 1 | BENCH cache_layer bank CS0 positions 1024 write_s 0.137 read_s 0.057 ms_per_position_read 0.056 rows_reread 0 bank_switches 1 |
| 1 | BENCH end temp_c 49.0 |
