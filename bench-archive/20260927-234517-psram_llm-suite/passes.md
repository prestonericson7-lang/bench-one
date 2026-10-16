# Passes over the weights -- D:\espicpc\bench-archive\20260927-234517-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: FIFO, card clock 66.0 MHz -- 10 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 105.8 (105.7-105.8) | 76.0 | 24.13 | 0.93 | 28.8 | 28.80 |
| 5 | 1 | 213.6 (213.6-213.6) | 82.9 | 22.12 | 4.42 | 126.2 | 25.24 |
| 8 | 5 | 280.4 (278.7-282.0) | 82.9 | 22.12 | 5.32 | 192.1 | 24.01 |

- total = 81.58 s + 24.95 s x slots (largest residual 7.26 s)
- M7 arithmetic = 5.86 s + 23.33 s x slots (largest residual 3.68 s)
- card = 75.30 s + 0.99 s x slots (largest residual 2.66 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.13 | 1.18 | 4.63 | 80.02 | 13.79 | 0.00 | 42.5 |
| 5 | 1 | 0.00 | 11.08 | 5.61 | 8.55 | 166.41 | 21.90 | 0.00 | 43.1 |
| 8 | 5 | 0.01 | 14.80 | 6.63 | 11.50 | 218.68 | 28.76 | 0.00 | 43.8 |

