# psram_llm suite -- 20261016-063554-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| chat_spi_i2c | 1 | 45 | 4 | 10 | 1356.4 | 135.64 | 63.47 | 289.1 | 1060.7 | 951.4 | 6.6 | 51.6 | 49/49 | none | 0 | -- | "SPI (Serial Peripheral Interface" |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 51.6 |
| 1 | BENCH sd_seq read_size 4096 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq read_size 16384 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq read_size 65536 mb_s 18.54 sdio fifo |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 18.54 sdio fifo clock_khz 66000 |
| 1 | BENCH sd_seq read_size 4096 mb_s 7.51 sdio dma |
| 1 | BENCH sd_seq read_size 16384 mb_s 7.51 sdio dma |
| 1 | BENCH sd_seq read_size 65536 mb_s 7.51 sdio dma |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 7.51 sdio dma clock_khz 66000 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.684 mb_s 5.71 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.24 mweights_s 114.16 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.45 mweights_s 105.23 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path widened rows 56 cols 2048 mb_s 60.14 mweights_s 112.12 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 183.48 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.753 mweights_s 104.29 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.421 mweights_s 135.71 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.593 mweights_s 167.01 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.31 mweights_s 77.10 |
| 1 | BENCH kernel ffn_down_L0 type 14 path widened rows 7 cols 11008 mb_s 59.71 mweights_s 76.32 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 121.49 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.45 mweights_s 78.54 |
| 1 | BENCH kernel attn_v_L0 type 14 path widened rows 39 cols 2048 mb_s 63.54 mweights_s 81.22 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 121.74 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.46 mweights_s 78.56 |
| 1 | BENCH kernel token_embd type 14 path widened rows 39 cols 2048 mb_s 63.58 mweights_s 81.27 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 121.76 |
| 1 | BENCH psram_raw bank Y2 mode single 0x0B write_mb_s 6.51 read_mb_s 2.84 wrong 0 |
| 1 | BENCH psram_plat bank Y2 verified_write_mb_s 2.01 read_mb_s 2.91 errors 0 |
| 1 | BENCH cache_layer bank CS0 positions 1024 write_s 0.141 read_s 0.061 ms_per_position_read 0.060 rows_reread 0 bank_switches 1 |
| 1 | BENCH end temp_c 50.3 |
