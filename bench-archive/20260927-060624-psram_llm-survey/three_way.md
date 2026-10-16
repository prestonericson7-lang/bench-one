# Fast path, float path, llama.cpp -- 20260927-060624-psram_llm-survey

60 prompts, greedy, same token ids. Identical token for token over every generated token:

| pair | identical answers |
|---|---|
| fast (the Teensy) vs float | 51 of 60 |
| fast (the Teensy) vs llama.cpp | 40 of 60 |
| float vs llama.cpp | 41 of 60 |

Where they part: first differing generated token, and the two sides' top-2 margins there.

| # | kind | tokens | fast/float | fast/llama | float/llama |
|---|---|---|---|---|---|
| 1 | fact | 41 | same | 20 (0.040/0.049) | 20 (0.003/0.049) |
| 2 | fact | 41 | same | 2 (0.075/0.083) | 2 (0.149/0.083) |
| 3 | fact | 41 | same | same | same |
| 4 | fact | 41 | same | same | same |
| 5 | fact | 41 | same | same | same |
| 6 | fact | 41 | same | same | same |
| 7 | fact | 41 | same | same | same |
| 8 | fact | 41 | same | 23 (0.182/0.067) | 23 (0.188/0.067) |
| 9 | fact | 41 | 0 (0.213/0.680) | 0 (0.213/0.661) | same |
| 10 | fact | 41 | same | same | same |
| 11 | arith | 41 | same | same | same |
| 12 | arith | 41 | 10 (0.066/0.022) | 16 (0.009/0.016) | 10 (0.022/0.107) |
| 13 | arith | 41 | same | 7 (0.140/0.076) | 7 (0.022/0.076) |
| 14 | arith | 41 | same | 4 (0.603/0.010) | 4 (0.135/0.010) |
| 15 | chat | 41 | same | same | same |
| 16 | chat | 4 | same | same | same |
| 17 | chat | 41 | same | same | same |
| 18 | chat | 41 | same | same | same |
| 19 | code | 41 | same | same | same |
| 20 | code | 41 | same | same | same |
| 21 | code | 41 | same | same | same |
| 22 | code | 41 | same | 1 (0.292/0.090) | 1 (0.329/0.090) |
| 23 | code | 41 | 24 (0.019/0.012) | 34 (0.061/0.102) | 24 (0.012/0.056) |
| 24 | code | 41 | same | same | same |
| 25 | code | 41 | same | same | same |
| 26 | code | 41 | same | 34 (0.006/0.302) | 34 (0.003/0.302) |
| 27 | code | 41 | same | 33 (0.070/0.095) | 33 (0.099/0.095) |
| 28 | code | 41 | same | same | same |
| 29 | chat | 41 | same | same | same |
| 30 | chat | 41 | same | same | same |
| 31 | chat | 41 | same | same | same |
| 32 | chat | 41 | same | same | same |
| 33 | chat | 36 | 17 (0.023/0.005) | 19 (0.293/0.018) | 17 (0.005/0.141) |
| 34 | chat | 41 | 24 (0.037/0.045) | same | 24 (0.045/0.167) |
| 35 | chat | 32 | same | 13 (0.161/0.050) | 13 (0.072/0.050) |
| 36 | chat | 9 | same | 6 (0.143/0.209) | 6 (0.032/0.209) |
| 37 | chat | 41 | same | 31 (0.077/0.023) | 31 (0.051/0.023) |
| 38 | chat | 41 | 32 (0.037/0.031) | same | 32 (0.031/0.300) |
| 39 | chat | 41 | same | same | same |
| 40 | chat | 9 | same | same | same |
| 41 | chat | 13 | same | same | same |
| 42 | chat | 2 | same | same | same |
| 43 | chat | 6 | same | same | same |
| 44 | chat | 20 | same | same | same |
| 45 | chat | 12 | same | same | same |
| 46 | chat | 20 | 4 (0.000/0.034) | 17 (0.061/0.003) | 4 (0.034/0.005) |
| 47 | chat | 2 | same | same | same |
| 48 | chat | 3 | same | same | same |
| 49 | chat | 8 | same | same | same |
| 50 | chat | 15 | same | same | same |
| 51 | chat | 41 | same | same | same |
| 52 | chat | 14 | same | same | same |
| 53 | chat | 15 | same | same | same |
| 54 | chat | 2 | same | same | same |
| 55 | chat | 24 | same | 0 (0.537/0.172) | 0 (0.789/0.172) |
| 56 | chat | 2 | 0 (0.181/0.046) | 0 (0.181/0.220) | same |
| 57 | chat | 12 | 0 (0.018/0.202) | 0 (0.018/0.545) | same |
| 58 | chat | 41 | same | 38 (0.017/0.358) | 38 (0.124/0.358) |
| 59 | chat | 41 | same | same | same |
| 60 | chat | 16 | same | same | same |
