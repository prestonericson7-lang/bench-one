# Qwen3-Next on the Teensy core -- PC verification, 2026-10-17 03:41

model `D:/start/models/Qwen3-Next-80B-A3B-Instruct-Q4_K_M.gguf`, 8 generated tokens a prompt

## 1. The state formats

- state mode 1, per token: IDENTICAL (13 lines)
- state mode 1, prompt batched: IDENTICAL (13 lines)
- state mode 2, per token: IDENTICAL (13 lines)
- state mode 2, prompt batched: IDENTICAL (13 lines)
- state mode 3, per token: IDENTICAL (13 lines)
- state mode 3, prompt batched: IDENTICAL (13 lines)

## 2. Every prompt (half state)

- prompt 0, per token: IDENTICAL (13 lines) -- 'The capital of France is'
- prompt 0, prompt batched: IDENTICAL (13 lines) -- 'The capital of France is'
- prompt 1, per token: IDENTICAL (13 lines) -- 'def is_prime(n):\n'
- prompt 1, prompt batched: IDENTICAL (13 lines) -- 'def is_prime(n):\n'
- prompt 2, per token: IDENTICAL (22 lines) -- 'Q: What is 17 + 25?\nA:'
- prompt 2, prompt batched: IDENTICAL (22 lines) -- 'Q: What is 17 + 25?\nA:'
- prompt 3, per token: IDENTICAL (17 lines) -- 'const add = (a, b) =>'
- prompt 3, prompt batched: IDENTICAL (17 lines) -- 'const add = (a, b) =>'
- prompt 4, per token: IDENTICAL (42 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibab'
- prompt 4, prompt batched: IDENTICAL (42 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibab'
- prompt 5, per token: IDENTICAL (38 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibab'
- prompt 5, prompt batched: IDENTICAL (38 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibab'
- prompt 6, per token: IDENTICAL (44 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibab'
- prompt 6, prompt batched: IDENTICAL (44 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibab'
- prompt 7, per token: IDENTICAL (71 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibab'
- prompt 7, prompt batched: IDENTICAL (71 lines) -- '<|im_start|>system\nYou are Qwen, created by Alibab'

## 3. Faults

- a bank dies (bank 0, write 2500): recovered, every line IDENTICAL
- a write lands wrong unseen (read-back off; bank 0, unverified write 1500): recovered, every line IDENTICAL

## 4. Units

- F16C half rounding: IDENTICAL; f16 self-test: 0 of 2^32 floats and 2^16 halves differ (7.6 s) -- the unit is used
- the M7 kernels, emulated, per token: IDENTICAL
- the M7 kernels, emulated, prompt batched: IDENTICAL

**EVERYTHING IDENTICAL**
