# Passes over the weights -- D:\espicpc\bench-archive\20260928-022632-psram_llm-suite\serial.log

39 passes: 0 one-position, 0 batched-prompt, 39 multi. Every number below was printed by the board.

## Card read: FIFO, card clock 66.0 MHz -- 39 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 9 | 121.5 (121.5-121.5) | 92.2 | 19.89 | 0.29 | 29.0 | 28.99 |
| 2 | 1 | 143.0 (143.0-143.0) | 83.3 | 22.04 | 0.50 | 59.2 | 29.60 |
| 3 | 6 | 165.0 (164.9-165.0) | 83.2 | 22.07 | 0.67 | 81.2 | 27.05 |
| 6 | 1 | 232.5 (232.5-232.5) | 83.0 | 22.11 | 1.20 | 148.3 | 24.72 |
| 8 | 22 | 275.8 (274.6-277.0) | 83.0 | 22.11 | 0.85 | 192.0 | 24.00 |

- total = 99.20 s + 22.08 s x slots (largest residual 1.22 s)
- M7 arithmetic = 8.32 s + 23.01 s x slots (largest residual 4.87 s)
- card = 90.60 s + -1.00 s x slots (largest residual 5.30 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 9 | 0.00 | 6.13 | 0.57 | 4.63 | 96.37 | 13.80 | 0.00 | 42.5 |
| 2 | 1 | 0.00 | 7.36 | 1.02 | 5.61 | 113.98 | 15.02 | 0.00 | 42.5 |
| 3 | 6 | 0.00 | 8.60 | 1.41 | 6.58 | 131.06 | 17.32 | 0.00 | 43.1 |
| 6 | 1 | 0.00 | 12.32 | 2.58 | 9.54 | 183.88 | 24.19 | 0.00 | 43.1 |
| 8 | 22 | 0.00 | 14.80 | 2.09 | 11.50 | 218.67 | 28.77 | 0.00 | 43.1 |

