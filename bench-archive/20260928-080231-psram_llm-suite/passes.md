# Passes over the weights -- D:\espicpc\bench-archive\20260928-080231-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: ADMA2, overlap 1, align 1, slice one row, card clock 66.0 MHz -- 6 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 5 | 1 | 143.9 (143.9-143.9) | 34.1 | 53.86 | 1.19 | 108.6 | 21.72 |
| 8 | 5 | 165.4 (164.3-166.5) | 1.9 | 987.17 | 0.95 | 162.6 | 20.32 |

- total = 108.07 s + 7.17 s x slots (largest residual 1.10 s)
- M7 arithmetic = 18.63 s + 17.99 s x slots (largest residual 0.48 s)
- card = 87.77 s + -10.73 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 5 | 1 | 0.01 | 7.95 | 2.41 | 5.93 | 109.75 | 17.84 | 107.10 | 43.8 |
| 8 | 5 | 0.01 | 8.98 | 2.27 | 6.68 | 125.75 | 21.70 | 150.07 | 43.8 |

## Card read: FIFO, card clock 66.0 MHz -- 4 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 105.1 (105.1-105.1) | 76.0 | 24.14 | 0.27 | 28.8 | 28.80 |

- total: every pass had the same slot count, no line to fit
- M7 arithmetic: every pass had the same slot count, no line to fit
- card: every pass had the same slot count, no line to fit

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.13 | 0.52 | 4.63 | 80.02 | 13.79 | 0.00 | 41.9 |

