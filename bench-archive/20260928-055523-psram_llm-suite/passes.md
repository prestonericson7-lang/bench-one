# Passes over the weights -- D:\espicpc\bench-archive\20260928-055523-psram_llm-suite\serial.log

39 passes: 0 one-position, 0 batched-prompt, 39 multi. Every number below was printed by the board.

## Card read: ADMA2, overlap 1, align 1, slice one row, card clock 66.0 MHz -- 29 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 3 | 6 | 142.7 (142.7-142.8) | 60.7 | 30.24 | 0.60 | 81.4 | 27.14 |
| 6 | 1 | 155.6 (155.6-155.6) | 5.9 | 313.21 | 1.07 | 148.6 | 24.77 |
| 8 | 22 | 194.7 (193.5-195.7) | 1.6 | 1124.24 | 0.76 | 192.3 | 24.04 |

- total = 109.99 s + 10.53 s x slots (largest residual 17.54 s)
- M7 arithmetic = 14.98 s + 22.16 s x slots (largest residual 0.80 s)
- card = 94.49 s + -11.67 s x slots (largest residual 18.54 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 3 | 6 | 0.00 | 7.90 | 1.34 | 5.93 | 109.72 | 17.84 | 80.48 | 44.4 |
| 6 | 1 | 0.01 | 9.01 | 2.45 | 6.81 | 119.45 | 17.84 | 138.62 | 43.8 |
| 8 | 22 | 0.01 | 11.43 | 2.01 | 8.77 | 151.95 | 20.52 | 145.68 | 43.8 |

## Card read: FIFO, card clock 66.0 MHz -- 10 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 9 | 121.4 (121.4-121.4) | 92.3 | 19.89 | 0.26 | 28.9 | 28.90 |
| 2 | 1 | 142.9 (142.9-142.9) | 83.2 | 22.04 | 0.45 | 59.3 | 29.65 |

- total = 99.90 s + 21.50 s x slots (largest residual 0.00 s)
- M7 arithmetic = -1.50 s + 30.40 s x slots (largest residual 0.00 s)
- card = 101.40 s + -9.10 s x slots (largest residual 0.00 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 9 | 0.00 | 6.13 | 0.54 | 4.62 | 96.32 | 13.79 | 0.00 | 41.9 |
| 2 | 1 | 0.00 | 7.37 | 0.97 | 5.61 | 113.96 | 15.03 | 0.00 | 41.9 |

