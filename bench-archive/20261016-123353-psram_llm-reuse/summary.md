# The attention cache reused across prompts (::reuse 1, ::gen 4)

`I bench-one psram_llm v9 banks 8 ps_bytes 67108864 max_seq 2608 gen 16 sdio dma overlap 1 align 1 slice 0 clk 99000 model qwen3b.gguf`

| prompt | tokens | positions reused | step lines printed vs tl_ref | prompt s (to the first answer token) | total s | die max C |
|---|---|---|---|---|---|---|
| 1 the question, cold cache | 45 | 0 | IDENTICAL (49 of 49) | 920.2 | 1092.7 | 54.2 |
| 2 another question, same system turn | 43 | 29 | IDENTICAL (18 of 47) | 291.6 | 464.0 | 54.2 |
| 3 conversation 1, turn 2 | 67 | 29 | IDENTICAL (42 of 71) | 784.2 | 957.5 | 54.2 |

All printed step lines match tl_ref on the whole prompt.
