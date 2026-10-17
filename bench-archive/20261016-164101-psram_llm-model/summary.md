# sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba on the board, every printed step against tl_ref (::gen 4)

`I bench-one psram_llm v9 banks 8 ps_bytes 67108864 max_seq 2608 gen 16 sdio dma overlap 1 align 1 slice 0 clk 99000 model qwen3b.gguf batch 8 experts 0/0`

| prompt | tokens | step lines vs tl_ref | first answer token s | each token after, s | card MB/s | compute s (hidden under reads) | die max C | answer |
|---|---|---|---|---|---|---|---|---|
| France | 36 | IDENTICAL (5 of 40) | 591.9 | 43.3 | 227.61 | 692.6 (488.6) | 51.6 | "The capital of France is" |
| chat_spi_i2c | 45 | IDENTICAL (5 of 49) | 309.3 | 43.5 | 185.35 | 412.1 (312.4) | 51.6 | "SPI (Serial Peripheral Interface" |

- `RUN n_prompt=36 n_gen=4 prefill=1 passes=9 total_s=765.3 prompt_s=591.9 per_pass_s=85.03 sd_gb=15.175 sd_mbs=227.61 sd_s=68.3 compute_s=692.6 hidden_s=488.6 ps_s=4.4 ps_fix_r=0 ps_fix_w=0 ps_bad=0 ps_sw=53 ps_sw_s=0.11 temp_max_c=51.6`
- `RUN n_prompt=45 n_gen=4 prefill=1 passes=7 total_s=483.1 prompt_s=309.3 per_pass_s=69.02 sd_gb=12.067 sd_mbs=185.35 sd_s=66.7 compute_s=412.1 hidden_s=312.4 ps_s=4.4 ps_fix_r=0 ps_fix_w=0 ps_bad=0 ps_sw=42 ps_sw_s=0.09 temp_max_c=51.6`

All printed step lines match tl_ref.
