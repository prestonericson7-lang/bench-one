# Passes over the weights -- D:\espicpc\bench-archive\20260928-052543-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: ADMA2, overlap 1, align 1, slice one row, card clock 66.0 MHz -- 6 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 5 | 1 | 144.5 (144.5-144.5) | 17.0 | 107.88 | 0.99 | 126.5 | 25.30 |
| 8 | 5 | 194.8 (193.7-195.8) | 1.6 | 1132.68 | 0.79 | 192.4 | 24.05 |

- total = 60.70 s + 16.76 s x slots (largest residual 1.08 s)
- M7 arithmetic = 16.73 s + 21.95 s x slots (largest residual 0.46 s)
- card = 42.67 s + -5.13 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 5 | 1 | 0.01 | 8.03 | 2.21 | 5.99 | 110.44 | 17.85 | 124.82 | 43.8 |
| 8 | 5 | 0.01 | 11.43 | 2.12 | 8.77 | 151.94 | 20.52 | 145.72 | 43.8 |

## Card read: FIFO, card clock 66.0 MHz -- 4 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 105.1 (105.1-105.1) | 76.0 | 24.13 | 0.23 | 28.8 | 28.80 |

- total: every pass had the same slot count, no line to fit
- M7 arithmetic: every pass had the same slot count, no line to fit
- card: every pass had the same slot count, no line to fit

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.14 | 0.48 | 4.63 | 80.03 | 13.79 | 0.00 | 41.9 |

