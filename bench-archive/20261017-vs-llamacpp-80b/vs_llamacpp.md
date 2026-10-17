# The reference against llama.cpp -- Qwen3-Next-80B-A3B-Instruct-Q4_K_M.gguf

Greedy, same GGUF, same prompt token ids. `ours` = tests/tl_ref.exe (model_q.c's fused path; what the Teensy
computes, and tests/tl_host.exe equals it to the bit). A parting shows each side's top-2 margin there.

| # | generated | identical tokens | first parting at | margin ours / llama.cpp | ours from there | llama.cpp from there |
|---|---|---|---|---|---|---|
| 0 | 17 | **all 17** | | | | |
| 1 | 17 | **all 17** | | | | |
| 2 | 17 | **all 17** | | | | |
| 3 | 17 | 0 | token 0 | 0.020 / 0.043 |  a + b;\x0A | [341, 220, 470, 264] |
| 4 | 17 | **all 17** | | | | |
| 5 | 2 | **all 2** | | | | |
| 6 | 17 | **all 17** | | | | |
| 7 | 17 | **all 17** | | | | |

**7 of 8 answers identical token for token; 1 of the 1 partings are at a margin under 0.2 on our side.**
