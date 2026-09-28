# psram_llm suite -- 20260928-013821-psram_llm-suite

Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.

## Runs

| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| chat_spi_i2c | 1 | 45 | 4 | 10 | 2010.2 | 201.02 | 22.88 | 801.9 | 1201.8 | 0.0 | 6.5 | 43.1 | 49/49 | none | 0 | -- | "SPI (Serial Peripheral Interface" |

## Benchmarks

| run | line |
|---|---|
| 1 | BENCH begin temp_c 43.1 |
| 1 | BENCH sd_seq read_size 4096 mb_s 7.50 sdio dma |
| 1 | BENCH sd_seq read_size 16384 mb_s 7.50 sdio dma |
| 1 | BENCH sd_seq read_size 65536 mb_s 7.50 sdio dma |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 7.50 sdio dma clock_khz 66000 |
| 1 | BENCH sd_seq read_size 4096 mb_s 18.52 sdio fifo |
| 1 | BENCH sd_seq read_size 16384 mb_s 18.53 sdio fifo |
| 1 | BENCH sd_seq read_size 65536 mb_s 18.53 sdio fifo |
| 1 | BENCH sd_seq_dtcm read_size 4096 mb_s 18.53 sdio fifo clock_khz 66000 |
| 1 | BENCH sd_random read_size 4096 count 256 ms_each 0.682 mb_s 5.72 fails 0 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path presum rows 56 cols 2048 mb_s 61.18 mweights_s 114.05 |
| 1 | BENCH kernel ffn_gate_L0 type 12 path dot_q rows 56 cols 2048 mb_s 56.42 mweights_s 105.18 |
| 1 | BENCH kernel_x8 ffn_gate_L0 type 12 vectors 8 mweights_s_per_vector 138.86 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 0 cycles_per_weight 5.764 mweights_s 104.09 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 1 cycles_per_weight 4.431 mweights_s 135.40 |
| 1 | BENCH q4k_stage ffn_gate_L0 stage 2 cycles_per_weight 3.602 mweights_s 166.56 |
| 1 | BENCH kernel ffn_down_L0 type 14 path dot_q rows 7 cols 11008 mb_s 60.27 mweights_s 77.04 |
| 1 | BENCH kernel_x8 ffn_down_L0 type 14 vectors 8 mweights_s_per_vector 121.16 |
| 1 | BENCH kernel attn_v_L0 type 14 path dot_q rows 39 cols 2048 mb_s 61.43 mweights_s 78.53 |
| 1 | BENCH kernel_x8 attn_v_L0 type 14 vectors 8 mweights_s_per_vector 121.43 |
| 1 | BENCH kernel token_embd type 14 path dot_q rows 39 cols 2048 mb_s 61.44 mweights_s 78.53 |
| 1 | BENCH kernel_x8 token_embd type 14 vectors 8 mweights_s_per_vector 121.43 |
| 1 | BENCH psram_raw bank Y2 mode single 0x0B write_mb_s 6.44 read_mb_s 2.83 wrong 0 |
| 1 | BENCH psram_plat bank Y2 verified_write_mb_s 2.01 read_mb_s 2.90 errors 0 |
| 1 | BENCH cache_layer bank CS0 positions 1024 write_s 0.138 read_s 0.058 ms_per_position_read 0.056 rows_reread 0 bank_switches 1 |
| 1 | BENCH end temp_c 42.5 |
