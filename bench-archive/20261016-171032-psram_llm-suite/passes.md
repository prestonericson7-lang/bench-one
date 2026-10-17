# Passes over the weights -- D:\espicpc\bench-archive\20261016-171032-psram_llm-suite\serial.log

7 passes: 0 one-position, 0 batched-prompt, 7 multi. Every number below was printed by the board.

## Card read: card mode not in the log -- 7 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 1 | 45.5 (45.5-45.5) | 16.3 | 112.43 | 0.11 | 29.1 | 29.10 |
| 2 | 3 | 51.2 (51.0-51.3) | 5.6 | 327.85 | 0.15 | 45.4 | 22.70 |
| 4 | 1 | 84.0 (84.0-84.0) | 4.6 | 399.93 | 0.26 | 79.1 | 19.77 |
| 8 | 2 | 148.2 (148.1-148.4) | 3.0 | 608.03 | 0.27 | 145.0 | 18.12 |

- total = 22.56 s + 15.61 s x slots (largest residual 7.32 s)
- M7 arithmetic = 12.37 s + 16.59 s x slots (largest residual 0.39 s)
- card = 10.06 s + -0.99 s x slots (largest residual 7.23 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | 0.00 | 2.65 | 0.27 | 1.90 | 35.13 | 5.57 | 28.80 | 50.9 |
| 2 | 3 | 0.00 | 2.88 | 0.42 | 2.04 | 39.16 | 6.68 | 32.27 | 51.6 |
| 4 | 1 | 0.00 | 4.60 | 0.80 | 3.39 | 64.52 | 10.64 | 42.55 | 50.3 |
| 8 | 2 | 0.01 | 8.03 | 1.17 | 6.06 | 114.60 | 18.39 | 63.87 | 49.6 |

