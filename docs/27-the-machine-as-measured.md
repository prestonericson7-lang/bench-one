# 27 — The machine, as specified by measurement

> **Corrected by document 45.** The 39.3 MB/s here is the portable scalar kernel: a Teensy 4.1 is
> a Cortex-M7 and `gguf_dot.c`'s hand-written vector path was NEON, which never applied to it. A
> Cortex-M7 DSP path measures **66.97 MB/s on Q4_K and 67.69 on Q6_K, 1.60x on the real 69/31 mix**,
> bit-identical to the reference. Figures derived from 39.3 below are therefore low by that factor.

One page. Every number here was measured on hardware on this desk or synthesized from this project's
own RTL. Where a figure is still a guess it says so. This is what to build and why.

---

## Who does what, and it is not close

| part | reads memory at | turns 4-bit into maths at | **actually sustains** | job |
|---|---|---|---|---|
| **Zynq FPGA fabric** | 2500 MB/s | 2290 MB/s | **2290 MB/s** | **all the arithmetic** |
| Luckfox Pico | 993 MB/s | 61 MB/s | 57 MB/s | move data, keep time |
| Teensy 4.1 + PSRAM | 33.9 MB/s | 39.3 MB/s | 21.4 MB/s | move data, hold small things |
| ESP32-S3 | 57.6 MB/s | ~11 MB/s (estimate) | ~9 MB/s | move data, sensors, display |
| Zynq's own ARM cores | 2500 MB/s | ~154 MB/s (estimate) | ~145 MB/s | **traffic control only** |

One FPGA board is **forty times** a Luckfox and **a hundred times** a Teensy. The microcontrollers are
not slow helpers at the same job; they are doing a different job.

**The ARM cores on the FPGA boards must not do the arithmetic.** Same two boards, same weights: fabric
gives 2.27 tokens a second, the ARM cores give 0.15. A fifteen-fold difference decided purely by which
half of the chip runs the multiply.

---

## Build decisions that came out of measurements

**Put 256 lanes in the FPGA, not 64.** Synthesized: 64 lanes is 4.9% of the chip, 256 is 18.9%, 512 is
37%. Fabric is not the scarce thing. At 256 lanes the core can eat 12.8 GB/s, five times what the
board's DDR3 delivers, so any memory you add later becomes tokens with no re-synthesis.

**Split the output head across boards by vocabulary.** Without it the 3B model does **not fit** your two
boards: 243 MB of head plus 15 layers on one and 20 on the other leaves one layer of thirty-six homeless.
Split in half it fits with room. Each board scores its own share of the word list and reports only its
best few candidates, tens of bytes instead of 594 KB.

**Read input embeddings off the disk, not out of memory.** The same table is used two ways: as a lookup
it needs one 4 KB row per token, which an SD card serves in a tenth of a millisecond. As the output head
it is a full matrix multiply and must be in fast memory. Putting the lookup on disk costs nothing and
frees 243 MB, which is five layers.

**Balance the layer split by cost, not by count.** Q4_K unpacks at 15.69 GB/s and Q6_K at 6.11, so a
Q6_K tensor costs 2.6× the same bytes of Q4_K. Splitting evenly by layer count made one node carry
2.1× the work and set the pace for everyone.

**Never use tensor parallelism.** Pipeline parallelism moves an 8 KB activation per hop against 49.2 MB
of weights per layer, five thousand to one. Tensor parallelism moves gigabytes per token. That ratio is
why the machine is possible at all.

---

## Wiring: do not buy the ethernet adapters yet

Measured between a Luckfox and this PC over nothing but the USB cable already plugged in:

| payload | one-way | throughput |
|---|---|---|
| 64 B | 148 µs | — |
| 8 KB, one activation | 437 µs | 18.7 MB/s |
| 196 KB, a prefill batch | 6.2 ms | 31.9 MB/s |

437 µs against a stage that takes 100 to 440 ms is under half a percent. **USB is already fast enough.**

The catch is shape, not speed. USB gadget mode needs a host, so many boards around one hub works and
board-to-board does not. Ethernet is what buys direct board-to-board wiring, and it buys speed nothing
needs yet. Spend the money on a DIMM-capable FPGA board instead.

---

## It has to serve several requests at once

Measured on four nodes running the real model:

| requests in flight | throughput | nodes busy |
|---|---|---|
| 1 | 1.55 tok/s | 23–27% |
| 4 | 5.25 tok/s | 82–96% |

With one conversation, only one node works at a time and the rest wait, because the next token cannot
start until the last one exists. **Pipeline parallelism at one request buys capacity and nothing else.**

So the machine is a multi-request server. Four users, or four prompts, or a batch job. Every
single-request figure in this project is a one-over-node-count figure and is labelled that way.

---

## Prefill and decode are two different machines

| | limited by | per token |
|---|---|---|
| decode | memory and unpacking, 1834 MB read | 232 ms |
| prefill, batched | arithmetic, 10.2 MB read | 95 ms |

Batched prefill reads each weight once for the whole prompt and hits 93% of this desktop's floating
point peak. That makes it pure arithmetic, which is the one thing a GPU is overwhelmingly good at:
roughly **170× this CPU**. So the two small GPUs in the brief have a clear job — prefill — and decode
belongs to the distributed memory.

---

## What the full fleet comes to

| | this desktop | the fleet in the brief |
|---|---|---|
| memory bandwidth | 29.40 GB/s | 166.05 GB/s |
| **bandwidth it can actually use** | **10.55 GB/s** | **43.82 GB/s** |
| independent nodes | 1 | 208 |

4.2× the usable bandwidth, and a desktop cannot be bought up to it at any price: its memory controller
is soldered and its core count fixed.

Sixteen FPGA boards supply 36.6 of that 43.8 GB/s. A hundred and twenty-eight Luckfoxes supply 7.4.
**The microcontrollers are 92% of the node count and 17% of the throughput.**

---

## Where it loses, stated plainly

- **On a 3B model this desktop wins**, 4.25 tokens a second against 2.27 for two boards. A model that
  fits one machine is the wrong test.
- **Capacity today is the real limit.** Two boards hold 2 GB of fast memory; the desktop has 48 GB. A
  70B model needs about forty boards to fit in RAM.
- **The 1 TB in the brief needs FPGA boards with DIMM sockets.** These do not have them: no FMC
  connector, the headers cannot make 1.5 V for SSTL15, and there are six grounds per thirty-two signals.
- **Per-watt is still an estimate**, roughly 13×, and wants a meter on both ends.

---

## The one number still missing

**The Zynq's real DDR3 bandwidth.** Every throughput figure above scales with it and 2500 MB/s is an
estimate. It is the first thing to measure when the boards land, and if it comes in low, everything here
moves with it.

Two things are ready and waiting: `tests/unpack_esp32` compiles and needs an ESP32-S3 plugged in to
replace the last processor estimate, and `tests/hop_bench` will measure a board-to-board hop the day
there is a link between two boards.
