# Passes over the weights -- D:\espicpc\bench-archive\20261016-120424-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: ADMA2, overlap 1, align 1, slice one row -- 5 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 8 | 5 | 161.9 (160.8-163.0) | 1.4 | 1281.53 | 0.89 | 159.6 | 19.95 |

- total: every pass had the same slot count, no line to fit
- M7 arithmetic: every pass had the same slot count, no line to fit
- card: every pass had the same slot count, no line to fit

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 8 | 5 | 0.01 | 8.86 | 2.21 | 6.84 | 123.58 | 20.41 | 94.27 | 52.9 |

## Card read: FIFO -- 5 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 43.1 (43.1-43.1) | 12.9 | 142.11 | 0.26 | 29.9 | 29.90 |
| 5 | 1 | 111.1 (111.1-111.1) | 3.8 | 487.78 | 1.11 | 106.3 | 21.26 |

- total = 26.10 s + 17.00 s x slots (largest residual 0.00 s)
- M7 arithmetic = 10.80 s + 19.10 s x slots (largest residual 0.00 s)
- card = 15.18 s + -2.27 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 2.65 | 0.51 | 1.90 | 32.48 | 5.56 | 29.32 | 53.5 |
| 5 | 1 | 0.00 | 6.03 | 2.33 | 4.51 | 84.38 | 13.88 | 51.39 | 52.9 |

