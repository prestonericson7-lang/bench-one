# PC verification of the Teensy core -- 20260929-215440-psram_llm-pcverify

## 1. Tokenizer

- corpus lines: 30338, 464749 tokens -- tl_core == tokenizer.c: **IDENTICAL**; tokfile: 30338 lines, 0 round-trip failures
- fuzz records: 2000, 92777 tokens -- tl_core == tokenizer.c: **IDENTICAL**; tokrec: 2000 records, 0 round-trip failures

## 2. Forward pass, 3 prompts, 12 generated tokens each

- prompt 0, per token: IDENTICAL (18 lines) -- 'The capital of France is'
- prompt 0, prompt batched: IDENTICAL (18 lines) -- 'The capital of France is'
- prompt 1, per token: IDENTICAL (18 lines) -- 'def is_prime(n):\n'
- prompt 1, prompt batched: IDENTICAL (18 lines) -- 'def is_prime(n):\n'
- prompt 2, per token: IDENTICAL (27 lines) -- 'Q: What is 17 + 25?\nA:'
- prompt 2, prompt batched: IDENTICAL (27 lines) -- 'Q: What is 17 + 25?\nA:'

## 3. Answered together (TL_MULTI)

- shared openings off: multi: 3 prompts, 16 passes over the weights for 60 positions (the same prompts one at a time with the batched prompt: 40 passes)
  - prompt 0 together: IDENTICAL
  - prompt 1 together: IDENTICAL
  - prompt 2 together: IDENTICAL
- shared openings on: multi: 3 prompts, 16 passes over the weights for 60 positions (the same prompts one at a time with the batched prompt: 40 passes)
  - prompt 0 together: IDENTICAL
  - prompt 1 together: IDENTICAL
  - prompt 2 together: IDENTICAL

## 3b. A PSRAM bank fails mid-run (TL_PS_FAULT: bank 3 dies on its 200th write)

- prompt 0, per token: IDENTICAL; no recovery line (rc 0)
- prompt 0, prompt batched: IDENTICAL; no recovery line (rc 0)
- all 3 prompts together: 3 identical after the fault (no retirement line (rc 0))

## 4. The M7 kernels, emulated on this PC (GD_EMULATE_M7)

- dot_verify_m7: single, presum and batched kernels against the scalar reference: IDENTICAL
- tl_host_m7, prompt 0, per token: IDENTICAL
- tl_host_m7, prompt 0, prompt batched: IDENTICAL

**ALL IDENTICAL**
