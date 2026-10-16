# The attention cache reused across prompts (::reuse 1, ::gen 4)

`I bench-one psram_llm v9 banks 8 ps_bytes 67108864 max_seq 2608 gen 4 sdio fifo overlap 1 align 1 slice 0 clk 99000 model qwen3b.gguf`

| prompt | tokens | positions reused | step lines printed vs tl_ref | prompt s (to the first answer token) | total s | die max C |
|---|---|---|---|---|---|---|
| turn 1 | 45 | 44 | IDENTICAL (5 of 49) | 45.3 | 217.9 | 54.2 |
| turn 2 | 67 | 49 | IDENTICAL (22 of 71) | 384.2 | 557.5 | 54.2 |
| turn 3 | 87 | 71 | IDENTICAL (20 of 91) | 331.7 | 505.8 | 54.2 |

All printed step lines match tl_ref on the whole prompt.
