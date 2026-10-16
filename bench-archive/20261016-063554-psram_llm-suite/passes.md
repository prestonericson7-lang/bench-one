# Passes over the weights -- D:\espicpc\bench-archive\20261016-063554-psram_llm-suite\serial.log

18 passes: 12 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: card mode not in the log -- 8 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 8 | 104.1 (104.1-104.2) | 69.2 | 26.50 | 0.10 | 34.8 | 34.80 |

- total: every pass had the same slot count, no line to fit
- M7 arithmetic: every pass had the same slot count, no line to fit
- card: every pass had the same slot count, no line to fit

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 8 | 0.00 | 6.05 | 0.26 | 4.59 | 79.53 | 13.72 | 34.31 | 51.6 |

## Card read: ADMA2, overlap 1, align 1, slice one row -- 5 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 8 | 5 | 162.4 (161.3-163.5) | 1.9 | 962.86 | 0.89 | 159.6 | 19.95 |

- total: every pass had the same slot count, no line to fit
- M7 arithmetic: every pass had the same slot count, no line to fit
- card: every pass had the same slot count, no line to fit

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 8 | 5 | 0.01 | 8.95 | 2.21 | 6.68 | 124.11 | 20.47 | 144.32 | 51.6 |

## Card read: FIFO -- 5 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 104.4 (104.4-104.4) | 69.2 | 26.51 | 0.26 | 34.9 | 34.90 |
| 5 | 1 | 126.7 (126.7-126.7) | 2.7 | 682.94 | 1.11 | 122.9 | 24.58 |

- total = 98.83 s + 5.57 s x slots (largest residual 0.00 s)
- M7 arithmetic = 12.90 s + 22.00 s x slots (largest residual 0.00 s)
- card = 85.82 s + -16.62 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.05 | 0.51 | 4.59 | 79.53 | 13.72 | 34.33 | 51.6 |
| 5 | 1 | 0.00 | 6.94 | 2.33 | 5.22 | 96.21 | 16.00 | 92.50 | 50.9 |

