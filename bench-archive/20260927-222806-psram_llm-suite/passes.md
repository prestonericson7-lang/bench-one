# Passes over the weights -- D:\espicpc\bench-archive\20260927-222806-psram_llm-suite\serial.log

3 passes: 2 one-position, 1 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: FIFO -- 3 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 2 | 112.5 (112.5-112.5) | 83.3 | 22.03 | 0.55 | 28.7 | 28.70 |
| 5 | 1 | 226.0 (226.0-226.0) | 98.0 | 18.73 | 2.55 | 125.5 | 25.10 |

- total = 84.12 s + 28.38 s x slots (largest residual 0.00 s)
- M7 arithmetic = 4.50 s + 24.20 s x slots (largest residual 0.00 s)
- card = 79.62 s + 3.68 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 2 | 0.00 | 6.59 | 0.66 | 4.95 | 85.52 | 14.76 | 0.00 | 42.5 |
| 5 | 1 | 0.00 | 12.13 | 3.07 | 9.40 | 176.91 | 24.48 | 0.00 | 43.1 |

