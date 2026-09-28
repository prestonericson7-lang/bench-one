# Passes over the weights -- D:\espicpc\bench-archive\20260928-013821-psram_llm-suite\serial.log

10 passes: 4 one-position, 6 batched-prompt, 0 multi. Every number below was printed by the board.

## Card read: FIFO, card clock 66.0 MHz -- 10 passes

| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 105.1 (105.1-105.1) | 76.0 | 24.14 | 0.25 | 28.8 | 28.80 |
| 5 | 1 | 210.2 (210.2-210.2) | 83.0 | 22.12 | 1.10 | 126.2 | 25.24 |
| 8 | 5 | 275.9 (274.8-277.0) | 83.0 | 22.11 | 0.88 | 192.1 | 24.01 |

- total = 81.41 s + 24.41 s x slots (largest residual 6.74 s)
- M7 arithmetic = 5.87 s + 23.33 s x slots (largest residual 3.69 s)
- card = 75.29 s + 1.00 s x slots (largest residual 2.70 s)

Stage split (mean seconds per pass, each stage including its own card reads):

| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0.00 | 6.13 | 0.51 | 4.63 | 80.02 | 13.79 | 0.00 | 42.5 |
| 5 | 1 | 0.00 | 11.08 | 2.32 | 8.55 | 166.38 | 21.91 | 0.00 | 43.1 |
| 8 | 5 | 0.01 | 14.80 | 2.20 | 11.50 | 218.65 | 28.76 | 0.00 | 43.1 |

