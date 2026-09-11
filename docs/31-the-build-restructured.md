# 31 — The build, restructured around the Lyras



Two Luckfox Lyra Ultra W boards replaced a pile of Picos. That is +1 GB of DDR3, +16 GB of eMMC, and
an Ethernet port with PoE on every one of them. It changes the machine more than the memory figure
suggests, and not in the direction I first assumed.

---

## What the Lyras actually are

**Not a speed upgrade. A capacity extension that costs speed when you lean on it.**

| | Zynq fabric | Lyra | ratio |
|---|---|---|---|
| effective throughput | 2290 MB/s | 170.7 MB/s | **13× slower** |
| memory | 1024 MB | 400 MB usable | |

Pipeline throughput is one over the slowest stage, so every layer a Lyra holds sets the pace for the
whole machine. Measured through the planner:

| context | with Lyras | Zynqs alone | layers per Lyra |
|---|---|---|---|
| 1,024 | 2.30 tok/s | 2.24 | 1 |
| 1,792 | 2.28 | 2.21 | 1 |
| **2,048** | **2.27** | **does not fit** | 1 |
| **4,096** | **2.20** | does not fit | 1 |
| 8,192 | 1.59 | does not fit | 2 |
| 16,384 | 0.98 | does not fit | 3 |

Read that as three regimes:

- **Under 1,792 tokens** the Lyras are a small bonus. They take one layer each off the Zynqs and buy
  about 3%.
- **From 2,048 to 4,096** they are the difference between running and not running. Before the trade the
  hard ceiling was 1,792 tokens of context. It is now 4,096 at a 4% speed cost.
- **Past 8,192** they start taking two and three layers each and the machine halves in speed.

**The design point is 4,096 tokens at 2.20 tok/s.** That is 2.3× the conversation length for 4% of the
throughput, and it is the best trade on the table.

---

## Roles, revised

| part | job | why |
|---|---|---|
| **2× Zynq PZ7020** | every layer they can hold, plus the output head split two ways | fabric unpacks 4-bit for free at 2290 MB/s; nothing else is close |
| **2× Lyra Ultra W** | 1 layer each, the input embedding lookup, tokenizer, sampling, and the network spine | 400 MB of RAM the Zynqs do not have, 8 GB of local eMMC, and the only Ethernet in the machine |
| **5× Luckfox Pico** | health monitoring, power sequencing, board bring-up | 13 MB cannot hold a 49.2 MB layer, so they carry no weights |
| **9× Teensy 4.1** | sensors, timing, the physical panel | 18.2 MB/s is 126× slower than a Zynq |
| **15× ESP32-S3** | display, buttons, environment, fan control | 9.2 MB/s, and its radio steals 0.55 ms whenever it feels like it |

The uncomfortable part, stated plainly: **the microcontrollers do no inference at all.** Giving a Teensy
one layer would make every token in the machine wait 2.3 seconds for it. They are the machine's nervous
system, not its muscle. That was true before the trade and the Lyras make it clearer, because now there
is a board in the middle that genuinely can carry weights and the gap between it and a Teensy is 9×
again.

### What the Lyras take over that nothing else could

- **The input embedding lookup off local eMMC.** The tied table is 243 MB and decode needs one 4 KB row
  of it per token. Reading that from 8 GB of onboard eMMC costs nothing and frees fast RAM on the Zynqs
  for weights, which is where the shortage is.
- **The tokenizer and sampling.** Small, serial, latency-insensitive work that would waste fabric.
- **The spine.** Every other board reaches the network through them.

---

## The wiring changes completely

Everything measured before this went over USB, and USB gadget mode always needed a host in the middle —
board-to-board was impossible. **The Lyras have RJ45 and PoE, so that constraint is gone.**

| link | per activation | scaling |
|---|---|---|
| WiFi 2.4 GHz | 2.60 ms | **shared** — every board divides one channel, gets worse as you add boards |
| **100M Ethernet** | **0.71 ms** | **switched** — every pair gets a private link, does not care how many boards |
| USB | 0.42 ms | fastest per link, but needs a host and cannot do board-to-board |

Ethernet is the spine. WiFi is the fallback for a board somewhere a cable will not reach. At 0.71 ms
against a 430 ms stage, the interconnect is 0.16% of a token and is not worth another thought until the
node count is much higher.

**PoE is the part to build around.** One cable per board carrying power and data halves the cabling and
removes a power supply per node, which is the difference between a rack that is maintainable and one
that is a nest. The whole machine is on the order of 30 W, so any small PoE switch covers it.

---

## What to buy, in order

1. **A PoE switch.** The one thing needed now. It turns two cables per board into one and it is what the
   Lyras were bought for whether or not that was the intent.
2. **An FPGA board with a DIMM socket.** The only purchase that raises the ceiling rather than the
   comfort. Everything above is bounded by 2 GB of fabric-attached memory; a DIMM board makes that 256.
3. **Nothing else.** Not Ethernet adapters — the Lyras have Ethernet. Not more Picos — they cannot hold
   a layer at any quantity. Not more Teensys or ESP32s for compute; they already outnumber their
   usefulness.

---

## Where this leaves the numbers

| | |
|---|---|
| fast memory | **3.10 GB** (2.0 fabric + 0.8 Lyra + 0.27 micro) |
| usable aggregate throughput | 5.38 GB/s |
| design point | 4,096 context, **2.20 tok/s**, 4 or more concurrent requests |
| interconnect cost | 0.16% of a token |
| the model that fits | Qwen2.5-Coder 3B at Q4_K/Q6_K, 36 layers |

And the same honest caveats as before, none of which the Lyras fixed:

- **This desktop still beats it on a 3B model**, 4.25 tok/s against 2.20. A model that fits one machine
  is the wrong test for a machine built to hold models that do not.
- **The Lyra figures are estimates**, scaled from the measured Pico by core count and clock. Scaled
  guesses in this project have been wrong by 2× before, twice. They need measuring on the board, and
  the binary to do it is already cross-compiled.
- **The Zynq's 2500 MB/s is still a guess** and every number here scales with it.

---

## The one structural idea worth chasing next

Mixture-of-experts, and the trade just made it more plausible rather than less.

An expert's slice of one layer is **2.9 MB**. A Pico holds four; a Lyra holds 137. Two Lyras hold 274
expert blocks where eleven Picos held 44 — **six times as many.** With Qwen3 Coder 30B reading only
2.1 GB of an 18.6 GB model per token, a 30B model costs about what the 3B costs per token.

The obstacle is routing traffic, and Ethernet just improved it from four seconds a token to one. Still
too slow, still fixed by grouping experts onto fewer nodes rather than by faster wire. That is the next
piece of work and it is a placement problem, not a hardware one.
