# 58 — Roadmap: what this small machine makes possible

This is the forward-looking document, so it needs a stricter rule than the others. Every statement here
carries one of three labels:

- **measured** — printed by hardware on this bench; the log is named in docs/54–57.
- **arithmetic** — computed from measured numbers; the computation is shown so it can be checked.
- **proposal** — not built; what would prove it is stated, and the number that must come out of it.

A rung of this roadmap is not climbed until its gate number has been measured. That is the rule the
project has run on since the first PSRAM bank (docs/40), and it is the reason the numbers here can be
trusted at all.

---

## 1. What has actually been shown — and what each fact means in general

The two days recorded in docs/57 were about one model on one board. Read with fresh eyes, each result
is an instance of something general.

| # | measured on this bench | what it means in general |
|---|---|---|
| F1 | A 1,929,903,072-byte model runs on a board with 1 MB of RAM. The model stays on a microSD card and all 1,922,981,520 bytes of weights are read for every token. Exact. | **A model does not have to be in memory to run.** RAM is working memory only. What a machine can run is set by its storage bandwidth, not by how much RAM it can afford. |
| F2 | Every step the board prints equals the PC to nine decimal places: nine 49-step chat runs, three eight-prompt runs, every France run since 2026-09-27 12:27. Cost of getting there: IEEE-double transcendentals (`tl_math.h`) and no fused multiplies. The rasteriser is likewise bit-exact between x86 and ARM (docs/32); the vocabulary-split output head is byte-identical to one process. | **Exactness across unrelated hardware is achievable and cheap.** A result can be re-derived on a different machine and compared byte for byte. That makes verification, redundancy and audit a comparison, not a statistic. |
| F3 | Chips Y0 and Y4 failed the start-up test on 12 of 24 boots. With one layer's cache per chip, a checksum beside every row and spare slots, no answer was ever wrong: on Sept 28, 8 rows re-read right, 2 writes redone, 0 unresolved. A simulated chip killed on its 200th write: output identical. | **Unreliable memory becomes reliable by layout, not by better chips.** Whole units of work per memory, a checksum per row, spare capacity, retire-and-rerun. |
| F4 | A pass feeding eight answers costs 193.5 s; feeding one, 105.1 s. Eight prompts answered together: 4.3× the tokens an hour. | **On a read-bound machine the read is a fixed cost and positions are nearly free.** One slow machine serves many people at once for the price of one. Queue, do not chat. |
| F5 | The card reads 24 MB/s in the mode that ties up the processor and 17.3 in the mode that frees it; 99 MHz is no faster than 66 (the card's limit); the Teensy's external bus ceiling is 66.5 MB/s by pin bonding; six extra chips on one bit-banged bus cost the good chips 49%. | **The walls are interfaces — wiring and protocol — not silicon.** The next gain is cheap; a new chip is not. |
| F6 | One-token arithmetic: 28.8 s over 1.83 GB = 15.7 ns per byte on the Cortex-M7. Batched Q4_K kernel 183.4 million multiply-adds a second. Two-bit weights: 1.96× the multiply-add rate of four-bit over the same bytes, bit-identical. 512 four-bit lanes synthesise into 37% of a Zynq 7020 (logic count only; not placed or timed). | **The design constants are known.** Anything built next can be sized on paper from these before a wire is cut. |

Three more, measured on the PC and carried for their *shape* only (PC speed never counts here,
docs/README):

| | measured (PC) | the shape it establishes |
|---|---|---|
| F7 | Running the 3B across four processes costs 0.05% of a token in network time; a real hop, Luckfox to PC over USB, is 437 µs for one 8 KB activation. | Distribution is cheap when only activations move. |
| F8 | Pipeline at one request in flight uses 1/n of the machine; the pipe held full reaches 82–96% busy. | **Pipelining buys capacity, not latency.** Every batch-1 figure is a 1/n figure. |
| F9 | A 30B mixture-of-experts touches 11% of itself per token; a cache of the hot experts hits 3–5× what a uniform guess would. | Sparse models are the only big models a streaming machine can run; and cold weights need no RAM at all. |

---

## 2. The idea that ties them together: compute at the storage

Put F1, F2 and F7 side by side. A Teensy with a card is **a storage module with a processor on it**: it
holds part of a model, streams that part to itself, does that part's arithmetic, and hands on an 8 KB
activation. Nothing else crosses a wire. On this board that is one module holding the whole model. Add
modules and the model's bytes never move; only activations do, and F7 says that costs nothing.

That is the architecture the largest plan in this project needs. docs/37 found that the link between
memory and processor stops being a constraint if the unpack happens *at* the memory — "even 1 GbE is
200× more than a module needs". The Teensy-and-card node is that module, built and measured, for about
$50. Everything below is that module made faster (Stage 1), made many (Stage 3), made of fabric (Stage
4) and made of discarded memory (Stage 5).

---

## 3. The technologies this opens

Each one names the facts it stands on and the thing still to be proven.

### T1 — Storage-streamed inference: the model as a file that is read, not loaded

*Stands on:* F1, F4, F5. *Exists today:* `shared/tl_core.c`, portable C, exact, with a five-function
platform contract (`tl_plat.h`) — a card read, an overlapped read, a memory read and write, a clock.

*The scaling rule (arithmetic):* tokens an hour = channels × bytes per second per channel ÷ bytes per
token. Every storage channel added adds bandwidth; the cards are $5 and the channels are wires. On this
board one channel gives 34 tokens an hour to one user and 4.3× that to eight.

*To prove next:* two channels on one board stream at once (Stage 1b).

### T2 — Verifiable inference: an answer anyone can re-derive

*Stands on:* F2. *Exists today:* `verify_pc.py` and `dot_verify.c` — the tokenizer, every kind of pass,
the board's own instructions emulated, and fault recovery, all required to be IDENTICAL; the rules that
make it so (`tl_math.h`, `fp-contract=off`, fixed integer accumulation order).

*What it gives:* (a) a cheap node can check an expensive one — a $30 board re-runs one step of a large
machine's work and either matches or does not; (b) a stated output can be shown to have come from a
stated model and input by anyone holding the file; (c) redundant nodes that disagree have detected a
fault, with no tolerance to argue about. A user of a cloud model today has none of the three: not the
file, not the board, not the digits.

*To prove next:* a third architecture — the Zynq's fabric engine — producing the same digits as the M7
and the PC for a whole token (Stage 4), and the rules written down as a one-page profile that any
implementation can be tested against.

### T3 — A memory fabric from unreliable parts

*Stands on:* F3, and the DDR3 bridge (docs/38–39: a scrapped desktop DIMM read as memory by a Teensy
through a $30 FPGA; 256 MB per eight data lines; **verified in simulation and synthesis, never on
hardware**).

*The pattern:* whole units of work per memory, never spanning two; a checksum beside every row; spare
capacity that absorbs a failed memory; retire and re-run. It cost the board 0.25 s of an 8-position
pass — 0.1% — to have it (measured).

*What it gives:* memory that may fail — salvaged, hand-soldered, hot, old — is usable as long as the
architecture expects it to. Y0 and Y4 have been proving that on this bench for two days.

*To prove next:* the pattern on a second kind of memory (the DIMM through the bridge, Stage 5), and a
chip pulled from the running board rather than killed in simulation.

### T4 — One slow machine, many people

*Stands on:* F4. *Exists today:* the `::multi` group pass — eight prompts, 39 passes, 8 of 8 exact, 1.81 h.

*The pattern:* questions are queued through the day and answered overnight; each answer is exact and
checkable. The queue front-end can be any of the boards on the shelf (the ESP32 module with the 4-inch
screen is on hand and does nothing else well).

*To prove next:* 24 hours of real questions with the board's power measured — tokens per day, tokens per
kilowatt-hour (Stage 2). **The board's power has not been measured**; it is the first missing number
under every claim in section 5.

### T5 — The streaming pipeline across cheap nodes

*Stands on:* F1, F7, F8, and the vocabulary split (byte-identical across processes, PC).

*Arithmetic for nine Teensys, each with its own card and a ninth of the layers:* 1,922,981,520 ÷ 9 =
213.7 MB per node per token; at the measured 24.13 MB/s that is 8.9 s, plus 28.8 ÷ 9 = 3.2 s of
arithmetic that cannot hide behind a FIFO read: **about 12 s per node per pass**. Pipelined with nine
requests in flight, one token every ~12 s of throughput; a single request still waits 9 × 12 ≈ 108 s a
token (F8: pipelining buys capacity). Cutting latency means splitting *inside* a layer, which exchanges an
8 KB activation several times per layer across a link — unmeasured on Teensy links, and docs/33's warning
that it costs more than it saves was for RAM, not time. *Assumptions not yet measured:* nine cards sustain
their rate at once (independent boards; plausible), and the Teensy-to-Teensy link rate.

*To prove next:* two boards, byte-identical to one (Stage 3).

### T6 — The same machine in fabric

*Stands on:* F5, F6, and the accelerator work in `accel/`: a fabric matrix engine (int4/int8 × int8,
batch 8) bit-exact against its testbench, in a merged bitstream that closes timing — **built, verified in
simulation and QEMU, not yet run on the board**. The PZ7020-StarLite: XC7Z020, 512 MB DDR3L, SD, USB
host, two gigabit Ethernet ports (one on the processor side, one on the fabric), vendor facts proven from
its manual and schematic (`hardware/pz7020-starlite/`).

*Arithmetic:* 256 lanes of four-bit multiply-add at 100 MHz is 25.6 billion a second; the 3B model's
3.09 billion per token takes 0.12 s. The arithmetic vanishes and the machine is purely read-bound: from
gigabit Ethernet at a nominal 100 MB/s, 1.92 GB is about 19 s a token; from the card slot or USB, whatever
those measure. 512 MB does not hold the model, so it streams (F1) and the DDR3 holds the attention cache
laid out as independent regions (F3) — the same design, faster.

*To prove next:* one whole token on the Zynq with digits equal to the PC and the Teensy; only then a time
(Stage 4).

### T7 — The memory that costs nothing: discarded DRAM, and the shed

*Stands on:* the DDR3 bridge (simulation), F3, F9, and the arithmetic in docs/36–37.

*Arithmetic, all of it docs/36–37:* DRAM is 12% of the core cost of a large machine; every DIMM is a
channel, so bandwidth grows with capacity; refresh, not work, sets the power (18.4 W per terabyte powered,
published IDD6), so the enclosure is a radiator and a 2 × 3 × 2.4 m shed dissipates 10,686 W passively
against 1,503 W of load. About $300k of parts holds 5.6–10.7 TB resident — a 10 to 20 trillion parameter
sparse model — at about 2.6 tokens a second to one user. "Not a rival for serving a model; a rival for
having one." The one number that decides it is the real cost of a carrier board that takes DIMMs; that
is 60–88% of the core and has not been measured.

*To prove next:* the bridge on hardware (Stage 5). Nothing in T7 is real until bytes come back from a DIMM
on this bench.

---

## 4. The roadmap, with gates

### Stage 0 — done (measured, docs/57)

One board, one card, eight chips: first answer token 955.5 s, 105.1 s a token, 177 prompt tokens an hour,
eight prompts in 1.81 h, exact on every step, chips failing and tolerated, die 41.9–50.9 °C.

### Stage 1 — the second channel (this board; weeks)

| step | what | label | gate |
|---|---|---|---|
| 1a | **Measure the board's power.** A USB meter in line during a pass. | proposal | watts, joules per token, tokens per kWh on the record |
| 1b | **A USB drive as a second storage channel**, half the model on it. The Teensy 4.1's USB host port is 480 Mb/s with its own DMA engine. If it streams at rate R while the card streams by ADMA2 at 17.3: read time = 1.92 GB ÷ (17.3 + R) MB/s. At R = 17.3 that is 55.6 s, with the 28.8 s of arithmetic hidden under it. | arithmetic; R unmeasured | one token under 60 s, digits equal to the PC |
| 1c | **Check the decoder's top address line** (Y0 and Y4 are its outputs 0 and 4, differing only there). | observation | continuity, five minutes |
| 1d | Two PSRAM buses of four instead of one of eight. Measured worth for this workload: **PSRAM is 0.1–0.2% of a pass; not worth doing for speed.** Its value would be chip count (context length) only. | measured | — |

The core does not know where its bytes come from (`plat_sd_read` is the platform's), so 1b changes the
board file and nothing in the proof. `verify_pc.py` runs unchanged before and after.

### Stage 2 — many people, one machine (weeks)

A queue front-end on a board already on the shelf; 24 hours of real questions through it; power from
1a. **Gate:** a day's log with every answer exact, and tokens per kilowatt-hour as a measured number.

### Stage 3 — two boards, then nine (months)

Layers split across Teensys, each with its own card (T5). Two first: measure the link, then the output
must be byte-identical to one board's (the test that caught every distributed bug on the PC, docs/33).
Then nine, for throughput. **Gate:** two boards identical to one; then tokens an hour on nine against the
12 s arithmetic above.

### Stage 4 — the fabric runs the same token (months)

The Zynq streams the model from its own storage or Ethernet, the fabric engine does the arithmetic, the
DDR3 holds the cache in independent regions. **Gate:** one token with digits equal to the PC and the
Teensy. Only then time it, and only then compare with the 19 s arithmetic. This stage also writes the
one-page exactness profile (T2), because the third implementation is what makes it a profile.

### Stage 5 — memory that costs nothing (a year or more)

The DDR3 bridge on hardware — a scrapped DIMM returning bytes to a Teensy. Then one layer's cache on it
with the checksum-and-spare pattern. Then the first measured constant for docs/36: what a DIMM carrier
actually costs to build. **Gate:** bytes back from a DIMM on this bench.

---

## 5. What it could change — each claim with what it rests on and what must pass first

| claim | rests on | must pass first |
|---|---|---|
| **AI where there is no data centre, no network and no subscription.** Three billion parameters of exact answers from about $50 of parts: a Teensy, a card, eight memory chips. Slow — a queue, not a chat — but private and owned. For the clinic, school, ship, or region where data must not leave the room or the connection does not exist. | F1, F4 (measured today) | nothing for the queue pattern; Stage 1–2 for usability and power |
| **Answers anyone can check.** A decision from this machine can be re-run by anyone with the model file and a $30 board and must give the same digits. | F2 (measured on two architectures) | Stage 4 (a third), and the written profile |
| **Compute from what is thrown away.** Chips that fail half the boots are tolerated (measured); DIMMs from scrapped PCs read as memory (simulated). | F3, docs/38 | Stage 5 |
| **A machine whose size is set by storage bandwidth and heat, not by RAM price.** The shed: 10–20 trillion parameters, passively cooled, for the price of a car — for *having* a model, not serving one. | docs/36–37 (arithmetic, conservative by 2–5× on its own accounting) | Stage 5, and the carrier-board cost |
| **A whole stack one person can own and understand.** Every line, wire and number is public (MIT). Nothing in it is a black box. | exists | — |

---

## 6. What this is not

- Not fast and not interactive: 16 minutes to the first word of an answer, 105 s a word after. Any
  phone runs this model faster. What the phone cannot do is show its digits equal a reference, run from
  a card with one megabyte of RAM, or keep working when half its memory fails.
- Not a smart model: 3B parameters. On the PC survey it answered 51 of 56 gradable questions right
  (code 10 of 10) and got 5 wrong. Exactness means the board gives the model's answer, not the right one.
- Not proven past Stage 0. Every figure in Stages 1–5 is arithmetic on measured numbers or a proposal,
  labelled as such, until a board prints its gate.

---

## 7. First actions, in order

1. A USB power meter in line; watts during a pass; joules per token on the record (1a).
2. The USB host path: a raw read-rate bench first, then half the model on a drive; one A/B (1b).
3. The decoder's A2 line, continuity (1c).
4. The 4-inch ESP32 module as a queue panel; a 24-hour run of real questions (Stage 2).
5. Two Teensys linked; output byte-identical to one (Stage 3).
6. The Zynq engine fed streamed weights; one exact token (Stage 4).
7. The DDR3 bridge on hardware (Stage 5).

Each is a single question with a single number as its answer, which is the only kind of step this
project takes.
