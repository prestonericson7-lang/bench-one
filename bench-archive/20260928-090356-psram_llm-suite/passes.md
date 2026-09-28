# Passes over the weights -- D:\espicpc\bench-archive\20260928-090356-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: ADMA2, overlap 1, align 1, slice one row, card clock 66.0 MHz -- 6 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 5 | 1 | 143.9 (143.9-143.9) | 37.6 | 48.86 | 1.26 | 105.1 | 21.02 |
| 8 | 5 | 162.5 (161.3-163.6) | 1.9 | 988.59 | 1.00 | 159.6 | 19.95 |

- total = 112.93 s + 6.19 s x slots (largest residual 1.18 s)
- M7 arithmetic = 14.20 s + 18.18 s x slots (largest residual 0.46 s)
- card = 97.10 s + -11.90 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 5 | 1 | 0.01 | 7.94 | 2.48 | 5.93 | 109.73 | 17.84 | 103.60 | 43.8 |
| 8 | 5 | 0.01 | 8.94 | 2.33 | 6.68 | 124.06 | 20.46 | 148.42 | 43.1 |

## Card read: FIFO, card clock 66.0 MHz -- 4 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 105.1 (105.1-105.1) | 76.1 | 24.11 | 0.29 | 28.7 | 28.70 |

- total: every pass had the same slot count, no line to fit
- M7 arithmetic: every pass had the same slot count, no line to fit
- card: every pass had the same slot count, no line to fit

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.13 | 0.54 | 4.63 | 80.02 | 13.79 | 0.00 | 41.9 |

