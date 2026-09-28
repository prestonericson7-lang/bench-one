# The reference model against llama.cpp -- 20260927-060624-psram_llm-survey

Greedy, same GGUF, same prompt token ids. `ours` = tests/tl_ref.exe (what the Teensy computes). A parting
is shown with each side's top-2 logit margin at that token.

| # | kind | generated | identical tokens | first parting at | margin ours / llama.cpp | ours from there | llama.cpp from there |
|---|---|---|---|---|---|---|---|
| 1 | fact | 41 | 20 | token 20 | 0.040 / 0.049 |  landmarks. Some of | [35005, 13, 576, 3283] |
| 2 | fact | 41 | 2 | token 2 | 0.075 / 0.083 |  It is the largest | [26194, 374, 279, 7772] |
| 3 | fact | 41 | **all 41** | | | | |
| 4 | fact | 41 | **all 41** | | | | |
| 5 | fact | 41 | **all 41** | | | | |
| 6 | fact | 41 | **all 41** | | | | |
| 7 | fact | 41 | **all 41** | | | | |
| 8 | fact | 41 | 23 | token 23 | 0.182 / 0.067 | 's walk on the | [572, 9223, 389, 6156] |
| 9 | fact | 41 | 0 | token 0 | 0.213 / 0.661 |  \"acronym\" | [409, 60163, 1897, 263] |
| 10 | fact | 41 | **all 41** | | | | |
| 11 | arith | 41 | **all 41** | | | | |
| 12 | arith | 41 | 16 | token 16 | 0.009 / 0.016 | 21\x0AD | [19, 15, 198, 35] |
| 13 | arith | 41 | 7 | token 7 | 0.140 / 0.076 |  given a question and | [458, 15235, 17847, 13] |
| 14 | arith | 41 | 4 | token 4 | 0.603 / 0.010 | B: 6 | [2610, 525, 458, 15235] |
| 15 | chat | 41 | **all 41** | | | | |
| 16 | chat | 4 | **all 4** | | | | |
| 17 | chat | 41 | **all 41** | | | | |
| 18 | chat | 41 | **all 41** | | | | |
| 19 | code | 41 | **all 41** | | | | |
| 20 | code | 41 | **all 41** | | | | |
| 21 | code | 41 | **all 41** | | | | |
| 22 | code | 41 | 1 | token 1 | 0.292 / 0.090 |  if xs == []:\x0A | [470, 2629, 53322, 692] |
| 23 | code | 41 | 34 | token 34 | 0.061 / 0.102 |  AND age >  | [401, 4858, 829, 4295] |
| 24 | code | 41 | **all 41** | | | | |
| 25 | code | 41 | **all 41** | | | | |
| 26 | code | 41 | 34 | token 34 | 0.006 / 0.302 |   # To avoid | [271, 286, 421, 2890] |
| 27 | code | 41 | 33 | token 33 | 0.070 / 0.095 |  the LED on\x0A | [389, 279, 13113, 198] |
| 28 | code | 41 | **all 41** | | | | |
| 29 | chat | 41 | **all 41** | | | | |
| 30 | chat | 41 | **all 41** | | | | |
| 31 | chat | 41 | **all 41** | | | | |
| 32 | chat | 41 | **all 41** | | | | |
| 33 | chat | 36 | 19 | token 19 | 0.293 / 0.018 | , ensuring that only | [13, 151645] |
| 34 | chat | 41 | **all 41** | | | | |
| 35 | chat | 32 | 13 | token 13 | 0.161 / 0.050 |  chip that can perform | [18250, 16224, 320, 1317] |
| 36 | chat | 9 | 6 | token 6 | 0.143 / 0.209 |  Memory.</im_end/> | [10612, 9549, 13850] |
| 37 | chat | 41 | 31 | token 31 | 0.077 / 0.023 |  data transfer modes and | [17646, 323, 821, 8317] |
| 38 | chat | 41 | **all 41** | | | | |
| 39 | chat | 41 | **all 41** | | | | |
| 40 | chat | 9 | **all 9** | | | | |
| 41 | chat | 13 | **all 13** | | | | |
| 42 | chat | 2 | **all 2** | | | | |
| 43 | chat | 6 | **all 6** | | | | |
| 44 | chat | 20 | **all 20** | | | | |
| 45 | chat | 12 | **all 12** | | | | |
| 46 | chat | 20 | 17 | token 17 | 0.061 / 0.003 |  friend.</im_end/> | [15254, 13, 151645] |
| 47 | chat | 2 | **all 2** | | | | |
| 48 | chat | 3 | **all 3** | | | | |
| 49 | chat | 8 | **all 8** | | | | |
| 50 | chat | 15 | **all 15** | | | | |
| 51 | chat | 41 | **all 41** | | | | |
| 52 | chat | 14 | **all 14** | | | | |
| 53 | chat | 15 | **all 15** | | | | |
| 54 | chat | 2 | **all 2** | | | | |
| 55 | chat | 24 | 0 | token 0 | 0.537 / 0.172 | The first five even | [15, 11, 220, 17] |
| 56 | chat | 2 | 0 | token 0 | 0.181 / 0.220 | Blue</im_end/> | [12203, 151645] |
| 57 | chat | 12 | 0 | token 0 | 0.018 / 0.545 | The word \"hello | [54048, 71, 151645] |
| 58 | chat | 41 | 38 | token 38 | 0.017 / 0.358 |  through its pads | [13, 151645] |
| 59 | chat | 41 | **all 41** | | | | |
| 60 | chat | 16 | **all 16** | | | | |

**40 of 60 answers identical token for token; 15 of the 20 partings are at a margin under 0.2 on our side.**
