# Passes over the weights -- D:\espicpc\bench-archive\20260927-122709-psram_llm-suite\serial.log

77 passes: 37 one-position, 1 batched-prompt, 39 multi. Every number below was printed by the board.

## Card read: FIFO -- 77 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 46 | 118.1 (113.9-134.8) | 87.0 | 21.23 | 1.00 | 30.1 | 30.12 |
| 2 | 1 | 166.6 (166.6-166.6) | 102.9 | 17.83 | 2.31 | 61.4 | 30.70 |
| 3 | 6 | 189.5 (189.4-189.6) | 102.7 | 17.86 | 3.17 | 83.6 | 27.87 |
| 5 | 1 | 233.5 (233.5-233.5) | 102.7 | 17.86 | 2.83 | 128.0 | 25.60 |
| 6 | 1 | 259.6 (259.6-259.6) | 102.7 | 17.86 | 5.72 | 151.1 | 25.18 |
| 8 | 22 | 303.9 (301.8-305.8) | 102.7 | 17.86 | 5.84 | 195.4 | 24.42 |

- total = 93.61 s + 26.51 s x slots (largest residual 19.96 s)
- M7 arithmetic = 7.21 s + 23.60 s x slots (largest residual 7.00 s)
- card = 86.03 s + 2.22 s x slots (largest residual 14.85 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 46 | 0.00 | 6.86 | 0.83 | 5.01 | 90.47 | 14.95 | 0.00 | 50.9 |
| 2 | 1 | 0.00 | 8.71 | 2.50 | 6.53 | 131.03 | 17.81 | 0.00 | 45.7 |
| 3 | 6 | 0.00 | 9.97 | 3.56 | 7.52 | 148.30 | 20.12 | 0.00 | 46.4 |
| 5 | 1 | 0.00 | 12.49 | 3.03 | 9.52 | 183.69 | 24.78 | 0.00 | 50.9 |
| 6 | 1 | 0.00 | 13.75 | 6.75 | 10.53 | 201.41 | 27.11 | 0.00 | 47.0 |
| 8 | 22 | 0.00 | 16.28 | 6.69 | 12.53 | 236.64 | 31.76 | 0.00 | 52.2 |

