# Passes over the weights -- D:\espicpc\bench-archive\20260927-211340-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: FIFO -- 10 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 113.0 (113.0-113.1) | 83.3 | 22.03 | 0.96 | 28.8 | 28.80 |
| 5 | 1 | 228.7 (228.7-228.7) | 98.0 | 18.73 | 4.60 | 126.2 | 25.24 |
| 8 | 5 | 295.5 (293.8-297.3) | 98.0 | 18.73 | 5.51 | 192.1 | 24.01 |

- total = 88.04 s + 26.08 s x slots (largest residual 10.24 s)
- M7 arithmetic = 5.87 s + 23.33 s x slots (largest residual 3.70 s)
- card = 81.80 s + 2.11 s x slots (largest residual 5.67 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.59 | 1.21 | 4.95 | 85.52 | 14.76 | 0.00 | 42.5 |
| 5 | 1 | 0.00 | 12.13 | 5.79 | 9.40 | 176.91 | 24.48 | 0.00 | 43.1 |
| 8 | 5 | 0.01 | 15.85 | 6.82 | 12.34 | 229.18 | 31.34 | 0.00 | 43.8 |

