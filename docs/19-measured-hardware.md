
# 19 — Measured hardware

Every figure here was taken from the actual boards on 2026-09-09, on a bench with heatsinks and
forced air. Anything still estimated is marked as such and says what it would take to measure it.

This exists because estimates in this project have been wrong in both directions and by large
factors. The Teensy came in **1.65× faster** than estimated and the ESP32-S3 **2.3× slower**. Two
errors that size in opposite directions is not bad luck, it is a reason to stop estimating.

---

## Nothing was thermally limited

| board | cooling | idle | under sustained load | throttle point | throttled? |
|---|---|---:|---:|---:|---|
| Teensy 4.1 | heatsink | 50.0 °C | 50.0 °C | — | no, held 600 MHz |
| Luckfox Pico Mini | heatsink, pads, fan, suspended | 45.8 °C | 53.5 °C peak | 75 °C | no, held 1104 MHz |
| ESP32-S3 | heatsink | 41.5 °C | 41.5 °C | — | no, held 240 MHz |

The Luckfox soak ran 150 seconds of continuous memory-bandwidth load and rose 7.7 °C, giving 2.4 °C
back within 30 seconds of the load stopping. It never left its top clock.

### These were taken in a hot room, which makes them a floor rather than a best case

Ambient during all of the above was roughly **30 °C** indoors, on a day around 38 °C outside. That
is close to worst-case domestic ambient, so the right way to read the table is as RISE OVER
AMBIENT, which is the quantity that stays put when the room or the enclosure changes:

| board | rise over ambient, idle | rise over ambient, loaded | in a 21 °C room |
|---|---:|---:|---:|
| Teensy 4.1 | 20 °C | 20 °C | ~41 °C |
| Luckfox Pico Mini | 16 °C | 24 °C | ~45 °C |
| ESP32-S3 | 12 °C | 12 °C | ~33 °C |

The consequence for the build: the Luckfox held 21.5 °C of margin to its first throttle point in a
30 °C room. A sealed enclosure typically adds 10 to 15 °C of internal ambient over the room, so
even a poorly ventilated box leaves margin, and a ventilated one is not close. The thermal design
is not the constraint and does not need to be revisited when these go in a case.

Every real limit found today was memory capacity, memory bandwidth, or clock policy. Not one was
heat. That is the practical difference between parts designed for the job and repurposed gaming
silicon that ships thermally throttled from the factory.

---

## The compare kernel

`hd_hamming` on an 8192-bit vector, which is the operation the whole machine is built from.

| board | clock | cycles/word | from fast memory | from bulk memory | penalty |
|---|---:|---:|---:|---:|---:|
| Teensy 4.1 | 600 MHz | **8.61** | 3.66 µs | 31.34 µs (PSRAM) | 8.6× |
| ESP32-S3 | 240 MHz | **46.07** | 49.14 µs | 60.07 µs (PSRAM) | 1.22× |
| Luckfox | 1104 MHz | *estimated* | *~1 µs* | — | — |

The Teensy is 5.3× cheaper per word because its Cortex-M7 has the DSP extension and `USAD8` finishes
the SWAR popcount in one instruction. Xtensa LX7 has no equivalent, so the ESP32 pays 46 cycles a
word. Same source file, same algorithm.

**The PSRAM penalty inverts between them, and the reason is worth keeping.** On the Teensy the core
is fast enough that memory dominates, so PSRAM costs 8.6×. On the S3 the core is slow enough that
compute dominates, so PSRAM is nearly free at 1.22×. An S3 should put its entire working set in
PSRAM without thinking about it; a Teensy has to weigh it.

The Luckfox number is the one gap. The board has no compiler and neither does the development PC.
Its CPU reports `neon vfpv4 edsp`, so the `USAD8` path in `bench_hdc.c` will compile for it, which
is the good case. Closing this needs a Windows-hosted `arm-none-linux-gnueabihf` cross-compiler
producing a static binary.

---

## Memory, and where it actually went

| board | headline | really available | hypervectors |
|---|---:|---:|---:|
| Teensy 4.1 | 1 MB internal + 16 MB PSRAM | 512 KB + 16 MB | ~1,000 + 16,384 |
| ESP32-S3 | 512 KB internal + 8 MB PSRAM | 285 KB free heap + 8 MB | ~278 + 8,192 |
| Luckfox Pico Mini | **64 MB DDR2** | **13,208 kB** | ~13,000 ceiling |

The Luckfox line is the correction that matters. The RV1103 carries 64 MB, but Rockchip reserves
most of it for the image and video engines before Linux boots, so the kernel only ever sees
33,560 kB and `MemAvailable` was 13,208 kB with the stock Buildroot image running. The stack model
had each Luckfox holding 20,000 hypervectors. That was never possible.

Both PSRAM installs were verified, not assumed: the Teensy's 16 MB passed a full write-read-compare
over all 4,194,304 words.

---

## microSD, measured in two different hosts

| | Teensy 4.1 | Luckfox |
|---|---:|---:|
| write | 9.7 MB/s | 10.1 MB/s |
| sequential read | 17.1 MB/s | 15.9 MB/s |
| random 1 KB, mean | 1,486 µs | 1,294 µs |
| random 1 KB, worst | 2,800 µs | **34,445 µs** |

The agreement between two completely different hosts is the useful part: the latency is the card,
not the machine, so this carries to any node with a card in it.

**Random access is 25× worse than sequential**, and that kills the obvious design. An index in PSRAM
addressing 839,000 bodies on a card costs 921 ms a query, past even the deep-tier deadline, with 90%
of that spent streaming the index rather than touching the card.

The way in is the sequential figure. Group similar vectors into contiguous runs, keep cluster
centroids in fast memory, and read whole runs instead of seeking per body. That is an ordinary
inverted-file index and it turns 25× against you into 25× for you.

The 34.4 ms worst-case fetch is not an outlier to ignore. It is two thirds of a 50 ms budget,
almost certainly the card's own garbage collection, and anything reading a card must survive it.
It is a direct argument for keeping the survivor cap low.

---

## The clock policy costs more than the heat

The Luckfox ships with the `ondemand` governor and **idles at 408 MHz**, one third of its 1104 MHz
top speed.

| governor | idle clock | time to full speed | cost of a cold query |
|---|---:|---:|---:|
| ondemand | 408 MHz | 13 ms | 2.7× |
| performance | 1104 MHz | 1 ms | 1.0× |

A fast-tier node is idle most of the time by design, so the first query after a quiet spell is the
common case, not the rare one, and 13 ms is 26% of a 50 ms deadline. Set `performance` on fast-tier
Luckfoxes. Leave `ondemand` on deep-tier ones, where 13 ms against a 500 ms budget is noise and the
idle power saving is free.

---

## The radio costs nothing measurable, and that is not the whole story

278,459 compares over 15 seconds on an ESP32-S3 while the wifi stack retried association: **longest
gap 0.55 ms, zero gaps past 1 ms**. Arduino runs on core 1 and the wifi stack on core 0, so a
compute loop is genuinely insulated.

That does not contradict the field experience that an nRF24 or CC1101 on an ESP32 transmits badly.
0.55 ms is invisible against a 50 ms deadline and catastrophic against a 130 µs PLL settle or an
8 µs PSRAM chip-enable window. **Jitter only matters relative to what it interrupts**, and a
throughput benchmark will never show it — only measuring the longest gap will.

The standing rule that follows: ESP32s do compute and their own 802.11 radio, and never drive a
peripheral whose correctness depends on software timing.

---

## What is still not measured

- **Luckfox compare cost.** Needs a cross-compiler. Everything else about that node is measured.
- **FPGA DDR3 bandwidth.** The logic is verified in simulation and synthesised at 797 logic cells,
  but no board has been powered.
- **PSRAM stick throughput.** `psram_bringup` will measure it on the first stick built.
- **Ethernet.** There is no `eth0` on the Luckfox. The on-die MAC and PHY are absent from the device
  tree, confirmed on hardware, so the only network today is USB. Ten nodes need this fixed.
