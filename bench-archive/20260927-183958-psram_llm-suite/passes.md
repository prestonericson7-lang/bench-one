# Passes over the weights -- D:\espicpc\bench-archive\20260927-183958-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: FIFO -- 10 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 113.2 (113.2-113.2) | 83.1 | 22.06 | 1.29 | 28.8 | 28.80 |
| 5 | 1 | 229.0 (229.0-229.0) | 97.8 | 18.75 | 5.05 | 126.2 | 25.24 |
| 8 | 5 | 295.8 (293.9-297.6) | 97.8 | 18.75 | 5.90 | 192.1 | 24.01 |

- total = 88.21 s + 26.09 s x slots (largest residual 10.32 s)
- M7 arithmetic = 5.87 s + 23.33 s x slots (largest residual 3.69 s)
- card = 81.60 s + 2.11 s x slots (largest residual 5.67 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.78 | 1.22 | 4.95 | 85.52 | 14.76 | 0.00 | 43.8 |
| 5 | 1 | 0.00 | 12.31 | 5.92 | 9.40 | 176.89 | 24.49 | 0.00 | 45.7 |
| 8 | 5 | 0.01 | 16.03 | 6.89 | 12.34 | 229.17 | 31.34 | 0.00 | 46.4 |

