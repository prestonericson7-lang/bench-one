# Int8 cache or int8 activations? -- 20260927-060624-psram_llm-survey

First differing generated token against float (`same` = identical throughout).

| # | fast vs float | fast + float cache vs float | fast + float cache vs fast |
|---|---|---|---|
| 1 | same | same | same |
| 2 | same | token 36 | token 36 |
| 3 | same | same | same |
| 4 | same | same | same |
| 5 | same | token 2 | token 2 |
| 6 | same | same | same |
| 7 | same | same | same |
| 8 | same | same | same |
| 9 | token 0 | same | token 0 |
| 10 | same | same | same |
| 11 | same | same | same |
| 12 | token 10 | same | token 10 |
| 13 | same | same | same |
| 14 | same | same | same |
| 15 | same | same | same |
| 16 | same | same | same |
| 17 | same | same | same |
| 18 | same | same | same |
| 19 | same | same | same |
| 20 | same | same | same |
| 21 | same | same | same |
| 22 | same | same | same |
| 23 | token 24 | same | token 24 |
| 24 | same | same | same |
| 25 | same | same | same |
| 26 | same | token 34 | token 34 |
| 27 | same | same | same |
| 28 | same | same | same |
| 29 | same | same | same |
| 30 | same | token 7 | token 7 |
| 31 | same | same | same |
| 32 | same | same | same |
| 33 | token 17 | token 17 | same |
| 34 | token 24 | token 24 | same |
| 35 | same | same | same |
| 36 | same | token 6 | token 6 |
| 37 | same | same | same |
| 38 | token 32 | token 32 | token 39 |
| 39 | same | same | same |
| 40 | same | same | same |
| 41 | same | same | same |
| 42 | same | same | same |
| 43 | same | same | same |
| 44 | same | same | same |
| 45 | same | same | same |
| 46 | token 4 | token 13 | token 4 |
| 47 | same | same | same |
| 48 | same | same | same |
| 49 | same | same | same |
| 50 | same | same | same |
| 51 | same | same | same |
| 52 | same | same | same |
| 53 | same | same | same |
| 54 | same | same | same |
| 55 | same | same | same |
| 56 | token 0 | same | token 0 |
| 57 | token 0 | same | token 0 |
| 58 | same | same | same |
| 59 | same | same | same |
| 60 | same | same | same |
