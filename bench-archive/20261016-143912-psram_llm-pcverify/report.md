# PC verification of the Teensy core -- 20261016-143912-psram_llm-pcverify

## 1. Tokenizer

- corpus lines: 31443, 488931 tokens -- tl_core == tokenizer.c: **IDENTICAL**; tokfile: 31443 lines, 0 round-trip failures
- fuzz records: 2000, 92777 tokens -- tl_core == tokenizer.c: **IDENTICAL**; tokrec: 2000 records, 0 round-trip failures

## 2. Forward pass, 3 prompts, 12 generated tokens each

- prompt 0, per token: IDENTICAL (18 lines) -- 'The capital of France is'
- prompt 0, prompt batched: IDENTICAL (18 lines) -- 'The capital of France is'
- prompt 1, per token: IDENTICAL (18 lines) -- 'def is_prime(n):\n'
- prompt 1, prompt batched: IDENTICAL (18 lines) -- 'def is_prime(n):\n'
- prompt 2, per token: IDENTICAL (27 lines) -- 'Q: What is 17 + 25?\nA:'
- prompt 2, prompt batched: IDENTICAL (27 lines) -- 'Q: What is 17 + 25?\nA:'

## 2b. Speculative decoding from the context (TL_LOOKUP, the board's ::lookup 6: last 4..2 tokens, 6 guesses)

- prompt 0: IDENTICAL; lookup: 14 passes over the weights for 17 positions (prompt 5), 3 draft tokens proposed, 0 accepted
- prompt 1: IDENTICAL; lookup: 14 passes over the weights for 17 positions (prompt 5), 0 draft tokens proposed, 0 accepted
- prompt 2: IDENTICAL; lookup: 12 passes over the weights for 26 positions (prompt 14), 6 draft tokens proposed, 4 accepted

## 2c. The batched pass's gate/up group (TL_FFN_GROUP; 256 is the default, run in 2)

- group 32, prompt batched: 3 of 3 prompts identical
- group 128, prompt batched: 3 of 3 prompts identical

## 2d. The output head only at each prompt's last position (TL_LOGITS_LAST, tl_set_logits_from)

- 3 of 3 prompts: every line from the prompt's last position on identical to tl_ref

## 3. Answered together (TL_MULTI), 4 at a time (the model's batch)

- shared openings off: 3 of 3 prompts identical
  - multi: 3 prompts, 21 passes over the weights for 60 positions (the same prompts one at a time with the batched prompt: 44 passes)
- shared openings on: 3 of 3 prompts identical
  - multi: 3 prompts, 21 passes over the weights for 60 positions (the same prompts one at a time with the batched prompt: 44 passes)

## 3b. A PSRAM bank fails mid-run (TL_PS_FAULT: bank 3 dies on its 200th write)

- prompt 0, per token: IDENTICAL; recovery: 4 lines re-run after the bank fault and compared with those printed before it: identical
- prompt 0, prompt batched: IDENTICAL; recovery: 0 lines re-run after the bank fault and compared with those printed before it: identical
- all 3 prompts together: 3 identical after the fault (PSRAM bank 3 retired, its layers moved to spare slots; re-running the prompt from position 0)

## 4. The M7 kernels, emulated on this PC (GD_EMULATE_M7)

- dot_verify_m7: single, presum and batched kernels against the scalar reference: IDENTICAL
- tl_host_m7, prompt 0, per token: IDENTICAL
- tl_host_m7, prompt 0, prompt batched: IDENTICAL

**ALL IDENTICAL**
