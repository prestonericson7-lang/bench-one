# sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba on the board, every printed step against tl_ref (::gen 4)

`I bench-one psram_llm v9 banks 8 ps_bytes 67108864 max_seq 2608 gen 4 sdio fifo overlap 1 align 1 slice 0 clk 99000 model qwen3b.gguf batch 8 experts 0/0`

| prompt | tokens | step lines vs tl_ref | first answer token s | each token after, s | card MB/s | compute s (hidden under reads) | die max C | answer |
|---|---|---|---|---|---|---|---|---|
| prompt 0 | 5 | IDENTICAL (5 of 9) | 134.6 | 72.0 | 41.11 | 199.0 (0.1) | 49.0 | " Paris. Paris is the" |

- `RUN n_prompt=5 n_gen=4 prefill=1 passes=5 total_s=422.5 prompt_s=134.6 per_pass_s=84.51 sd_gb=8.959 sd_mbs=41.11 sd_s=223.1 compute_s=199.0 hidden_s=0.1 ps_s=0.4 ps_fix_r=0 ps_fix_w=0 ps_bad=0 ps_sw=30 ps_sw_s=0.06 temp_max_c=49.0`

All printed step lines match tl_ref.
