# Passes over the weights -- D:\espicpc\bench-archive\20260928-045737-psram_llm-suite\serial.log

9 passes: 6 one-position, 3 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: FIFO, card clock 66.0 MHz -- 3 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 2 | 104.8 (104.8-104.8) | 76.0 | 24.14 | 0.04 | 28.7 | 28.70 |
| 5 | 1 | 208.6 (208.6-208.6) | 83.0 | 22.11 | 0.11 | 125.5 | 25.10 |

- total = 78.85 s + 25.95 s x slots (largest residual 0.00 s)
- M7 arithmetic = 4.50 s + 24.20 s x slots (largest residual 0.00 s)
- card = 74.25 s + 1.75 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 2 | 0.00 | 6.14 | 0.16 | 4.63 | 80.04 | 13.79 | 0.00 | 41.9 |
| 5 | 1 | 0.00 | 11.09 | 0.64 | 8.56 | 166.45 | 21.90 | 0.00 | 42.5 |

## Card read: ADMA2, overlap 1, align 1, slice one row, card clock 66.0 MHz -- 3 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 2 | 137.5 (137.5-137.5) | 108.5 | 16.91 | 0.04 | 28.9 | 28.90 |
| 5 | 1 | 143.0 (143.0-143.0) | 17.0 | 107.72 | 0.11 | 125.8 | 25.16 |

- total = 136.12 s + 1.38 s x slots (largest residual 0.00 s)
- M7 arithmetic = 4.68 s + 24.22 s x slots (largest residual 0.00 s)
- card = 131.38 s + -22.87 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 2 | 0.00 | 7.84 | 0.16 | 5.92 | 105.68 | 17.85 | 28.49 | 44.4 |
| 5 | 1 | 0.00 | 8.03 | 0.64 | 5.99 | 110.48 | 17.85 | 124.84 | 43.8 |

## Card read: ADMA2, overlap 0, align 1, slice one row, card clock 66.0 MHz -- 3 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 2 | 159.9 (159.9-159.9) | 131.1 | 14.00 | 0.04 | 28.7 | 28.70 |
| 5 | 1 | 260.9 (260.9-260.9) | 135.2 | 13.57 | 0.11 | 125.5 | 25.10 |

- total = 134.65 s + 25.25 s x slots (largest residual 0.00 s)
- M7 arithmetic = 4.50 s + 24.20 s x slots (largest residual 0.00 s)
- card = 130.08 s + 1.02 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 2 | 0.00 | 9.19 | 0.16 | 6.93 | 122.71 | 20.87 | 0.00 | 43.8 |
| 5 | 1 | 0.01 | 14.73 | 0.64 | 11.38 | 203.54 | 30.59 | 0.00 | 43.8 |

