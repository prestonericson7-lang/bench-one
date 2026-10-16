# Passes over the weights -- D:\espicpc\bench-archive\20260928-082914-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: ADMA2, overlap 1, align 1, slice one row, card clock 66.0 MHz -- 6 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 5 | 1 | 143.8 (143.8-143.8) | 37.4 | 49.03 | 1.14 | 105.3 | 21.06 |
| 8 | 5 | 162.8 (161.7-163.9) | 1.9 | 986.22 | 0.90 | 160.0 | 20.00 |

- total = 112.10 s + 6.34 s x slots (largest residual 1.12 s)
- M7 arithmetic = 14.07 s + 18.25 s x slots (largest residual 0.46 s)
- card = 96.57 s + -11.83 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 5 | 1 | 0.01 | 7.94 | 2.36 | 5.93 | 109.74 | 17.84 | 103.74 | 43.8 |
| 8 | 5 | 0.01 | 8.94 | 2.23 | 6.67 | 124.36 | 20.59 | 148.75 | 43.8 |

## Card read: FIFO, card clock 66.0 MHz -- 4 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 105.1 (105.1-105.1) | 76.0 | 24.14 | 0.26 | 28.8 | 28.80 |

- total: every pass had the same slot count, no line to fit
- M7 arithmetic: every pass had the same slot count, no line to fit
- card: every pass had the same slot count, no line to fit

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.13 | 0.51 | 4.63 | 80.01 | 13.79 | 0.00 | 42.5 |

