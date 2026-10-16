# PC verification of the Teensy core -- 20261016-124252-psram_llm-pcverify

## 1. Tokenizer

- corpus lines: 30934, 478998 tokens -- tl_core == tokenizer.c: **IDENTICAL**; tokfile: 30934 lines, 0 round-trip failures
- fuzz records: 20000, 965110 tokens -- tl_core == tokenizer.c: **IDENTICAL**; tokrec: 20000 records, 0 round-trip failures

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

## 2b. Speculative decoding from the context (TL_LOOKUP, the board's ::lookup 6: last 4..2 tokens, 6 guesses)

- prompt 0: IDENTICAL; lookup: 13 passes over the weights for 17 positions (prompt 5), 0 draft tokens proposed, 0 accepted
- prompt 1: IDENTICAL; lookup: 13 passes over the weights for 17 positions (prompt 5), 0 draft tokens proposed, 0 accepted
- prompt 2: IDENTICAL; lookup: 13 passes over the weights for 26 positions (prompt 14), 5 draft tokens proposed, 1 accepted
- prompt 3: IDENTICAL; lookup: 10 passes over the weights for 21 positions (prompt 9), 4 draft tokens proposed, 4 accepted
- prompt 4: IDENTICAL; lookup: 16 passes over the weights for 46 positions (prompt 34), 6 draft tokens proposed, 1 accepted
- prompt 5: IDENTICAL; lookup: 6 passes over the weights for 38 positions (prompt 37), 0 draft tokens proposed, 0 accepted
- prompt 6: IDENTICAL; lookup: 13 passes over the weights for 44 positions (prompt 36), 0 draft tokens proposed, 0 accepted
- prompt 7: IDENTICAL; lookup: 14 passes over the weights for 75 positions (prompt 63), 8 draft tokens proposed, 6 accepted

## 2c. The batched pass's gate/up group (TL_FFN_GROUP; 256 is the default, run in 2)

- group 32, prompt batched: 8 of 8 prompts identical
- group 128, prompt batched: 8 of 8 prompts identical

## 2d. The output head only at each prompt's last position (TL_LOGITS_LAST, tl_set_logits_from)

- 8 of 8 prompts: every line from the prompt's last position on identical to tl_ref

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

## 3b. A PSRAM bank fails mid-run (TL_PS_FAULT: bank 3 dies on its 200th write)

- prompt 0, per token: IDENTICAL; recovery: 5 lines re-run after the bank fault and compared with those printed before it: identical
- prompt 0, prompt batched: IDENTICAL; recovery: 5 lines re-run after the bank fault and compared with those printed before it: identical
- all 8 prompts together: 8 identical after the fault (PSRAM bank 3 retired, its layers moved to spare slots; re-running the prompt from position 0)

## 4. The M7 kernels, emulated on this PC (GD_EMULATE_M7)

- dot_verify_m7: single, presum and batched kernels against the scalar reference: IDENTICAL
- tl_host_m7, prompt 0, per token: IDENTICAL
- tl_host_m7, prompt 0, prompt batched: IDENTICAL
- tl_host_m7, all 8 prompts together (batched kernels, shared openings): 8 identical

**ALL IDENTICAL**
