# psram_llm suite -- 20260928-101322-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| chat_spi_i2c | 1 | 45 | 4 | 10 | 1375.8 | 137.58 | 52.28 | 350.9 | 1018.3 | 845.6 | 6.5 | 43.8 | 49/49 | none | 0 | -- | "SPI (Serial Peripheral Interface" |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 43.1 |
| 1 | BENCH sd_seq read_size 4096 mb_s 7.51 sdio dma |
| 1 | BENCH sd_seq read_size 16384 mb_s 7.51 sdio dma |
| 1 | BENCH sd_seq read_size 65536 mb_s 7.51 sdio dma |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 7.51 sdio dma clock_khz 66000 |
| 1 | BENCH sd_seq read_size 4096 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq read_size 16384 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq read_size 65536 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 18.55 sdio fifo clock_khz 66000 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.681 mb_s 5.73 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.25 mweights_s 114.17 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.40 mweights_s 105.14 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path widened rows 56 cols 2048 mb_s 60.14 mweights_s 112.10 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 183.47 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.756 mweights_s 104.25 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.423 mweights_s 135.65 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.594 mweights_s 166.93 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.25 mweights_s 77.02 |
| 1 | BENCH kernel ffn_down_L0 type 14 path widened rows 7 cols 11008 mb_s 59.72 mweights_s 76.33 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 121.50 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.39 mweights_s 78.47 |
| 1 | BENCH kernel attn_v_L0 type 14 path widened rows 39 cols 2048 mb_s 63.53 mweights_s 81.21 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 121.73 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.40 mweights_s 78.49 |
| 1 | BENCH kernel token_embd type 14 path widened rows 39 cols 2048 mb_s 63.57 mweights_s 81.26 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 121.76 |
| 1 | BENCH psram_raw bank Y0 mode single 0x03 write_mb_s 4.72 read_mb_s 1.78 wrong 0 |
| 1 | BENCH psram_plat bank Y0 verified_write_mb_s 1.31 read_mb_s 1.81 errors 0 |
| 1 | BENCH cache_layer bank CS0 positions 1024 write_s 0.142 read_s 0.061 ms_per_position_read 0.060 rows_reread 0 bank_switches 1 |
| 1 | BENCH end temp_c 41.9 |
