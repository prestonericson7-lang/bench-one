# PC verification of the Teensy core -- 20260927-105333-psram_llm-pcverify

## 1. Tokenizer

- corpus lines: 27963, 415433 tokens -- tl_core == tokenizer.c: **IDENTICAL**; tokfile: 27963 lines, 0 round-trip failures
  - against llama.cpp on the same text: 0 of 27963 differ
- fuzz records: 20000, 965110 tokens -- tl_core == tokenizer.c: **IDENTICAL**; tokrec: 20000 records, 0 round-trip failures
  - against llama.cpp on the same text: 0 of 20000 differ

## 2. Forward pass, 8 prompts, 12 generated tokens each

- prompt 0, per token: IDENTICAL (18 lines) -- 'The capital of France is'
- prompt 0, prompt batched: IDENTICAL (18 lines) -- 'The capital of France is'
- prompt 1, per token: IDENTICAL (18 lines) -- 'def is_prime(n):\n'
- prompt 1, prompt batched: IDENTICAL (18 lines) -- 'def is_prime(n):\n'
- prompt 2, per token: IDENTICAL (27 lines) -- 'Q: What is 17 + 25?\nA:'
- prompt 2, prompt batched: IDENTICAL (27 lines) -- 'Q: What is 17 + 25?\nA:'
- prompt 3, per token: IDENTICAL (22 lines) -- 'const add = (a, b) =>'
- prompt 3, prompt batched: IDENTICAL (22 lines) -- 'const add = (a, b) =>'
- prompt 4, per token: IDENTICAL (47 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. Y'
- prompt 4, prompt batched: IDENTICAL (47 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. Y'
- prompt 5, per token: IDENTICAL (39 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. Y'
- prompt 5, prompt batched: IDENTICAL (39 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. Y'
- prompt 6, per token: IDENTICAL (45 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. Y'
- prompt 6, prompt batched: IDENTICAL (45 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. Y'
- prompt 7, per token: IDENTICAL (76 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. Y'
- prompt 7, prompt batched: IDENTICAL (76 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. Y'

## 3. Answered together (TL_MULTI)

- shared openings off: multi: 8 prompts, 45 passes over the weights for 284 positions (the same prompts one at a time with the batched prompt: 110 passes)
  - prompt 0 together: IDENTICAL
  - prompt 1 together: IDENTICAL
  - prompt 2 together: IDENTICAL
  - prompt 3 together: IDENTICAL
  - prompt 4 together: IDENTICAL
  - prompt 5 together: IDENTICAL
  - prompt 6 together: IDENTICAL
  - prompt 7 together: IDENTICAL
- shared openings on: multi: 8 prompts, 36 passes over the weights for 212 positions (the same prompts one at a time with the batched prompt: 110 passes)
  - prompt 5 copies its first 24 positions from prompt 4
  - prompt 6 copies its first 24 positions from prompt 4
  - prompt 7 copies its first 24 positions from prompt 4
  - prompt 0 together: IDENTICAL
  - prompt 1 together: IDENTICAL
  - prompt 2 together: IDENTICAL
  - prompt 3 together: IDENTICAL
  - prompt 4 together: IDENTICAL
  - prompt 5 together: IDENTICAL
  - prompt 6 together: IDENTICAL
  - prompt 7 together: IDENTICAL

## 4. The M7 kernels, emulated on this PC (GD_EMULATE_M7)

- dot_verify_m7: single, presum and batched kernels against the scalar reference: IDENTICAL
- tl_host_m7, prompt 0, per token: IDENTICAL
- tl_host_m7, prompt 0, prompt batched: IDENTICAL
- tl_host_m7, all 8 prompts together (batched kernels, shared openings): 8 identical

**ALL IDENTICAL**
