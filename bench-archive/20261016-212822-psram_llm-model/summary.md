# sha256-1194192cf2a187eb02722edcc3f77b11d21f537048ce04b67ccf8ba78863006a on the board, every printed step against tl_ref (::gen 4)

`I bench-one psram_llm v9 banks 8 ps_bytes 67108864 max_seq 1126 gen 16 sdio dma overlap 1 align 1 slice 0 clk 99000 model qwen30b.gguf batch 4 experts 8/128`

| prompt | tokens | step lines vs tl_ref | first answer token s | each token after, s | card MB/s | compute s (hidden under reads) | die max C | answer |
|---|---|---|---|---|---|---|---|---|
| France | 36 | IDENTICAL (5 of 40) | 856.2 | 44.5 | 175.11 | 813.7 (567.3) | 55.5 | "The capital of France is" |
| chat_spi_i2c | 45 | IDENTICAL (5 of 49) | 438.5 | 44.7 | 166.56 | 471.7 (344.9) | 56.1 | "SPI (Serial Peripheral Interface" |

- `RUN n_prompt=36 n_gen=4 prefill=1 passes=13 total_s=1034.1 prompt_s=856.2 per_pass_s=79.54 sd_gb=35.597 sd_mbs=175.11 sd_s=208.2 compute_s=813.7 hidden_s=567.3 ps_s=12.2 ps_fix_r=0 ps_fix_w=0 ps_bad=0 ps_sw=91 ps_sw_s=0.19 temp_max_c=55.5`
- `RUN n_prompt=45 n_gen=4 prefill=1 passes=9 total_s=617.2 prompt_s=438.5 per_pass_s=68.58 sd_gb=21.722 sd_mbs=166.56 sd_s=133.5 compute_s=471.7 hidden_s=344.9 ps_s=12.0 ps_fix_r=0 ps_fix_w=0 ps_bad=0 ps_sw=63 ps_sw_s=0.13 temp_max_c=56.1`

All printed step lines match tl_ref.
