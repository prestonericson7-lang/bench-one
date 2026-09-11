# 09 — Scaling: what 100 PSRAM chips actually buys

> **Superseded in part by doc 10 (2026-09-08).** The arithmetic here still stands, but the
> inventory and roles changed: 60 PSRAM (not 100), **6 per Teensy on a bank-switched backpack**
> (not 12 on the fabric's decoder), two Zynq boards with 1 GB DDR3 each as the Teensys' big
> memory, and Teensyduino 1.62 clocks the QSPI at 105.6 MHz, not 88. Use doc 10 §2 for the
> per-Teensy bandwidth numbers.

Every number here shows its working so it can be attacked.

**The short version.** Storage was never the problem — one trillion weights fits on a $20 microSD
card. **Bandwidth is the problem, and sparsity is the fix**, which is exactly what every frontier
model already does. What your parts buy is roughly **4 billion ternary weights with a sub-second
forward pass**, and a **~100 million weight** network you can genuinely train from scratch
overnight.

---

## 1. The chip, verified

[ESP-PSRAM64H datasheet](https://www.espressif.com/sites/default/files/documentation/esp-psram64_esp-psram64h_datasheet_en.pdf):

| | |
|---|---|
| Capacity | 64 **Mbit** = **8 MByte** |
| Clock | 133 MHz — **but 84 MHz for bursts crossing a 1K page boundary** |
| Interface | SPI or QPI (4-bit) |
| Supply | 3.3 V |
| Page size | 1 KB, address A[22:0] |

That page-boundary derating is the spec that matters here. Streaming a large weight array crosses
pages constantly, so **84 MHz is the real number, not 133.**

```
84 MHz × 4 bits = 336 Mbit/s = 42 MB/s per chip, sequential
100 chips × 8 MB = 800 MB total
```

---

## 2. How many weights 800 MB holds

The precision choice is the whole ballgame.

| Format | Bits/weight | Weights in 800 MB |
|---|---|---|
| fp32 | 32 | 200 million |
| int8 | 8 | 800 million |
| int4 | 4 | 1.6 billion |
| **ternary (BitNet b1.58)** | **1.58** | **4.05 billion** |
| binary | 1 | 6.4 billion |

**Ternary is the right choice and it is not a compromise.** Microsoft's BitNet b1.58 showed
weights constrained to {−1, 0, +1} matching full-precision transformers from 3B parameters upward.
Three properties make it near-perfect for this machine:

- **1.58 bits/weight** — five times more model in the same PSRAM than int8.
- **No multiplier needed.** A ternary weight means add, subtract, or skip. On the M7 that is an
  add instead of a MAC; the multiply disappears entirely.
- **It makes the 74HC fabric genuinely useful for arithmetic** for the first time. Add / subtract /
  skip is trivial discrete logic. Everything before this was decoration.

---

## 3. The trillion-weight question, answered directly

**Storage is trivial and cheap.**

```
1 × 10^12 weights × 1.58 bits = 1.58 × 10^12 bits = 197.5 GB
```

That fits on **one 256 GB microSD card, about $20**, and the Teensy 4.1 has an SD socket on board.
You do not need 24,700 PSRAM chips to *hold* a trillion weights. You never did.

**Bandwidth is the wall, and here is the number.**

A dense forward pass must read every weight exactly once. So one token costs 197.5 GB of reads.

| Machine | Memory bandwidth | Time for one dense trillion-weight pass |
|---|---|---|
| NVIDIA H100 | 3.35 TB/s | 0.06 s |
| RTX 4090 | 1.0 TB/s | 0.2 s |
| **Your 100-chip PSRAM array** | **~1 GB/s** (see §4) | **197 s** |
| One microSD | ~20 MB/s | 2.7 hours |

Three minutes per token from PSRAM. Nearly three hours from SD. That is the honest arithmetic, and
no amount of cleverness changes the division.

**But dense is not how trillion-weight models actually run.**

DeepSeek-V3 is 671 billion total parameters and activates **37 billion** per token. Mixtral 8×7B is
47 billion total, 13 billion active. This is Mixture of Experts, and it exists precisely because
nobody can afford to read every weight for every token — not even on an H100.

So the real architecture is a memory hierarchy, and you already own every tier:

| Tier | Medium | Size | Holds |
|---|---|---|---|
| Cold | microSD | 256 GB | the full trillion, ternary |
| **Hot** | **100× PSRAM** | **800 MB** | **~4 billion active-expert weights** |
| Fast | 8× Teensy DTCM | ~3 MB | the layer being computed right now |

That is structurally the same as a GPU's SSD → HBM → SRAM hierarchy, and it is exactly how
`llama.cpp` runs models larger than your RAM today.

---

## 4. The bandwidth arithmetic, which is the real design

**Aggregate bandwidth scales with the number of independent BUSES, not the number of chips.**

A hundred chips on one shared SPI bus is still one bus — 42 MB/s total, and the other 99 chips
just wait. This is the single most important engineering fact on the page.

Each Teensy 4.1 has three SPI peripherals plus two dedicated QSPI pads.

```
8 Teensys × 3 usable SPI buses  = 24 independent buses
24 buses × 42 MB/s              = ~1.0 GB/s aggregate
```

| Model size (ternary) | Bytes to read, dense | Time at 1.0 GB/s |
|---|---|---|
| 800 M weights | 158 MB | 0.16 s |
| **4 billion weights** | **790 MB** | **0.79 s** |
| 4 billion, 10% MoE-active | 79 MB | **0.08 s → ~12 tokens/s** |

**A four-billion-weight ternary network with a sub-second forward pass, on about $600 of parts.**
With sparsity, roughly twelve tokens per second.

That is not a toy. That is GPT-2-XL scale and beyond, running locally.

### Where the 100 chips physically go

Twelve or thirteen chips per Teensy, each Teensy owning its own bus, with a **74HC138 decoder
generating the chip selects.**

```
'138 gives 8 selects from 4 pins.
Two cascaded gives 64. Three tiers gives 512.
100 chips needs 7 address bits — trivial.
```

**This is the first job in the whole AI machine that the fabric is genuinely the right tool for.**
Not decoration, not nostalgia — 100 chip selects is precisely what a decoder tree is for, and you
own 24 of them.

---

## 5. Training versus running, because they are not the same problem

This is where I have to be straight with you.

**Running a large model: yes, as above.**

**Training a large model from scratch: no, and here is the number rather than an opinion.**

```
Chinchilla-optimal training for 1T parameters ≈ 6 × N × D FLOPs
                                              ≈ 6 × 10^12 × 20×10^12  ≈ 1.2 × 10^26 FLOPs

8 Teensys × 1.2 GMAC/s ≈ 2 × 10^10 FLOP/s

1.2 × 10^26 / 2 × 10^10 = 6 × 10^15 seconds ≈ 190 million years
```

That is not a limitation of your design. Frontier training runs use tens of thousands of H100s for
months and cost upward of $100M. The gap is compute, not cleverness, and it is 15 orders of
magnitude.

**What you CAN train from scratch, with the arithmetic:**

```
Training ≈ 3× forward cost.
8 Teensys × 1.2 GMAC/s = 9.6 GMAC/s → ~3.2 G weight-updates/s

One epoch = 3 × samples × weights MACs

100 M weights, 60,000 samples:
  3 × 6×10^4 × 1×10^8 = 1.8 × 10^13 MACs
  1.8 × 10^13 / 9.6 × 10^9 = 1,875 s = 31 minutes per epoch
```

**~100 million weights, half an hour per epoch, trained overnight.** That is a real network — larger
than the original AlexNet's fully-connected layers — trained from nothing on hardware you own.

And your deep belief net makes this *better*, because greedy layer-wise pretraining trains one RBM
at a time. Each Teensy trains its own layer independently and ships features onward. The links
carry activations, never gradients.

---

## 6. Your actual argument, taken seriously

You said $219k is still billions less than frontier models. That is right, and it understates it.

| | Cost |
|---|---|
| Frontier training run | $100,000,000+ |
| Your 4-billion-weight machine | **~$600** |
| A trillion weights of ternary storage | **~$20** (one microSD) |

The honest framing is not "a cheap supercomputer." It is:

> **A 4-billion-parameter ternary neural machine with a sub-second forward pass, built from
> $1.75 memory chips and $32 microcontrollers, with 100 chip-selects decoded by 1970s logic.**

Nobody has published that. It is not a reproduction of anything.

---

## 7. What to buy, and in what order

**Do not order 100 chips yet.** Order **8** and prove one bank first.

The thing that will bite is not the chips, it is signal integrity: a QSPI bus at 84 MHz fanning out
to twelve chips on a PCB is a real transmission-line problem, and if it does not work at twelve it
does not work at a hundred. Prove one Teensy with twelve chips at full speed, on the analyser,
before committing $175.

| Order | Why |
|---|---|
| 8× ESP-PSRAM64H | ~$14. Prove one bank at 84 MHz before scaling |
| 1× 256 GB microSD | Cold tier. The Teensy already has the socket |
| 74HC138 | You own 24 |

Two things to measure on the first bank, both of which decide the whole build:

1. **Real sustained MB/s** from one chip with page-crossing bursts. My 42 MB/s is derived from the
   datasheet, not measured.
2. **How many chips one bus tolerates** before edges degrade. This sets chips-per-Teensy, and
   therefore the whole machine's size.

---

## 8. What I am least sure of

- **42 MB/s per chip.** Derived from 84 MHz × 4 bits. Real QSPI has command and address overhead
  per transaction, so sustained will be lower. Measure it.
- **Three usable SPI buses per Teensy.** SPI0 currently drives the display and SPI2 is on
  bottom-side pads. The true count may be two per board, which halves aggregate bandwidth.
- **1.2 GMAC/s per Teensy.** From `SMLAD` at two MACs/cycle. Ternary needs no multiply at all, so
  the real ternary rate may be *higher* — an add is one cycle.
- **BitNet at this scale.** b1.58 was validated at 3B+ parameters on transformers. Whether ternary
  holds up for a deep belief net at 100M is genuinely unknown, and would itself be a result worth
  publishing.
