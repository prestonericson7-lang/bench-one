# The reference against llama.cpp -- sha256-1194192cf2a187eb02722edcc3f77b11d21f537048ce04b67ccf8ba78863006a

Greedy, same GGUF, same prompt token ids. `ours` = tests/tl_ref.exe (model_q.c's fused path; what the Teensy
computes, and tests/tl_host.exe equals it to the bit). A parting shows each side's top-2 margin there.

| # | generated | identical tokens | first parting at | margin ours / llama.cpp | ours from there | llama.cpp from there |
|---|---|---|---|---|---|---|
| 0 | 17 | 14 | token 14 | 0.205 / 0.464 |  Italy?\x0AThe | [64871, 5267, 785] |
| 1 | 17 | **all 17** | | | | |
| 2 | 17 | **all 17** | | | | |
| 3 | 17 | **all 17** | | | | |
| 4 | 17 | 15 | token 15 | 1.451 / 0.938 | **\x0A3 | [334, 715] |
| 5 | 12 | **all 12** | | | | |
| 6 | 17 | **all 17** | | | | |
| 7 | 17 | **all 17** | | | | |

**6 of 8 answers identical token for token; 0 of the 2 partings are at a margin under 0.2 on our side.**
