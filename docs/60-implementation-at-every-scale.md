# 60 — Implementation at every scale, and how it changes AI

docs/58 said where this leads and docs/59 derived the equations. This document answers the practical
question: **how is it built, at what scales, out of what, producing what — and by what sequence does a
$50 board change how AI is done.** Same three labels on every line: **measured** (on this bench, logs
named in docs/54–57), **arithmetic** (computed from measured numbers, shown), **proposal** (not built;
what would prove it is stated). Published maker figures are marked as such. Every product of factors
is something to distrust until each factor is measured alone (docs/59 §7).

---

## 1. The ladder

Six scales. The architecture is the same at every one — a model in flash streamed to a processor beside
it, working memory in RAM laid out as independent checksummed units, exactness proven by comparison —
because the three facts it rests on (docs/58 F1–F3) do not depend on size.

| scale | what it is | parts | cost | power | runs | to one user | to a crowd | status |
|---|---|---|---|---|---|---|---|---|
| **0 — the module** | one Teensy 4.1, one card, eight PSRAM | on hand | ~$50 | ~0.5 W (maker's published draw) | 3B exact; 30B at the same rate (docs/59 L2) | 105 s a token today; 14–56 s with the levers | 180 tokens an hour | **measured** today's numbers; levers arithmetic |
| **1 — the shelf** | eight or nine modules and a panel board | on hand: 10 Teensy, 60 PSRAM, ESP32 with a 4-inch screen | ~$450 | ~4.5 W | 3B / 30B | ~15 s a token (eight boards split inside each matrix) | 1,620 tokens an hour, ~39,000 a day | arithmetic |
| **2 — the fabric node** | one Zynq 7020 board streaming weights, computing in fabric, cache in its DDR3 | on hand: 2 | ~$150 | under 5 W (supply rating) | 3B / 30B | ~19 s a token at a nominal 100 MB/s | ~30,000 tokens an hour | arithmetic, several factors unmeasured |
| **3 — the bench machine** | 2 fabric nodes + 9 modules + 6 Luckfox + ESP32s: the inventory | on hand | ~$1,500 total parts already bought | ~20 W | 3B / 30B, many users | 15–19 s | ~60,000 an hour, ~1.4 million a day | arithmetic on top of arithmetic |
| **4 — the shed** | DIMM-carrier core, module layer, passive cooling (docs/36) | to buy | ~$300k | ~1.5 kW | 10–20 trillion parameter sparse model | 2.6 tokens a second | multiplies with positions | arithmetic (docs/36), conservative by 2–5× on its own accounting |
| **5 — the network** | modules anywhere; models on cards; transcripts checked by strangers | anyone's | $50 each | 0.5 W each | anything that fits a card | — | as many as there are modules | proposal |

Scale 0 exists and is measured. Every other row is that row's parts multiplied by the equations of
docs/59 §1, and the rest of this document says what has to be built, and proven, to make each row real.

---

## 2. Scale 0 — the module: what to build in it

Everything at this scale is firmware, and every item is proven the same way: **the step lines must be
identical to the PC's before the time is read** (`verify_pc.py`, docs/54).

| item | what changes | where | proof | worth (arithmetic, docs/59) |
|---|---|---|---|---|
| **L1 speculative decoding from the prompt** | a drafter that proposes the continuation last seen after the current tokens in the context; the batched pass verifies k + 1 positions; rejected cache rows are overwritten from the accepted position | `tl_core.c` (a few hundred lines); no board-file change | identical step lines to a normal run — greedy speculation is exact by construction | 105 → 52 s a token at α = 0.7; α measured on the PC first |
| **L2 the 30B mixture-of-experts** | the PC core already parses and runs it (`model_q.c`, docs/33); the board core needs the expert gather (8 blocks a layer through the O(1) seeks), the router, and a 48-layer PSRAM layout at 2.7× the cache a position | `tl_core.c`, `gguf.c`; a 32 GB card | same test as the 3B: every step equal to `tl_ref` on the same file | ten times the model at about the same token time; a third of the positions |
| **L4 per-tensor width** | a converter that writes each tensor at the width its measured sensitivity allows; a 2-bit kernel for the card path (the bus kernel exists, docs/48) with its `dot_verify` check | `gguf_dot.c`, a PC tool | `dot_verify` bit-identical; perplexity on the yardstick | halves the read where it is used |
| **L5 the second channel** | `plat_sd_read` served by two devices, the model's bytes split between card and a USB drive on the host port; both by DMA | board file only; the core does not know | identical step lines; then the rate | 105 → ~56 s a token at a matching drive rate |
| **L3 the stall budget** | one build with the cycle counter around each instruction group of one row | `gguf_dot.c` | the counts | decides whether the kernel has 1.5× left |
| **the panel** | a queue: questions in, answers out, on the ESP32 module with the 4-inch screen already on the shelf; or any serial host | new sketch | a day's log, every answer exact | usability (docs/58 Stage 2) |

Order: L1's α, L2's byte counts and L4's perplexities on the PC first — they are model properties and
cost no hardware — then one board build each. **The module with L1, L2 and L5 is the 30-billion-
parameter model answering one person in under a minute a token, exact, on $50 of parts** — arithmetic;
three measurements away.

**The module as a board.** The owner can design and order PCBs. The module wants one: the Teensy's
processor or a socketed Teensy; a microSD slot on the four-bit SDIO; a USB host connector; eight PSRAM
on **two buses of four** (measured: six extra chips on one bus cost the good ones 49%, docs/48) each
behind its own decoder; a link header for the shelf (§3); 5 V in. Cost target under $50 in tens. That
board is the unit everything above scale 0 is made of, and a stranger can build one from the wiring
sheet (docs/47) and the firmware as they stand today. *Label:* proposal.

---

## 3. Scale 1 — the shelf: eight or nine modules

Two arrangements, both from parts on hand (10 Teensy, 60 PSRAM: seven eight-chip modules, or ten
six-chip ones at 1,956 positions each — measured, docs/57 §8).

**For a crowd: independent modules.** Each holds its own copy of the model and its own queue. Nine of
them: 9 × 180 = 1,620 tokens an hour, about 39,000 a day, about 1,200 answers of 32 tokens — on 4.5 W
(nine times the maker's published draw), which a 10 W solar panel and a small battery cover
(arithmetic; the power is the first measurement, docs/58 Stage 1a). Nothing to build but the panel and
the power trunk: 5 V at about 1 A for nine boards.

**For one person: the split group.** Eight boards each read their own rows of every matrix from their
own card and exchange the 8 KB activation (docs/59 §1.4): about 15 s a token, arithmetic. What has to be
built: the link — the Teensy 4.1 has eight hardware serial ports (6 Mb/s each, published) and three SPI
ports, so a ring or a star needs no extra silicon — and the small protocol document (docs/59 I4): the
40-byte stage header that already exists on the PC (docs/25), the activation format, the two split
modes. **Proof, before any timing: two boards produce step lines identical to one board's.** That is
the test that caught every distributed bug on the PC (docs/25) and it is the gate here.

The two compose: a shelf of nine can run as one group of eight for latency and one spare, or as nine
queues for throughput, by configuration.

---

## 4. Scale 2 — the fabric node

The PZ7020-StarLite: a Zynq 7020 with 512 MB of DDR3, an SD slot, a USB host port and two gigabit
Ethernet ports (one on the processor side, one wired to the fabric), facts from its manual and schematic
(`hardware/pz7020-starlite/`). The fabric matrix engine in `accel/` is bit-exact against its testbench
and closes timing in the merged bitstream — **built and simulated, not yet run on the board.**

What the equations say when the arithmetic term vanishes (256 four-bit lanes at 100 MHz: 3.09 billion
multiply-adds in 0.12 s):

    read at 100 MB/s:  B / bw = 19 s          one user: ~19 s a token
    positions that hide under it:  P* = 19 / 0.12 ≈ 160
    crowd:  160 positions per 19 s pass ≈ 8 tokens a second ≈ 30,000 an hour

The DDR3 holds the cache: a 384 MB region at 18.0 KB a position (the 3B at int8, docs/29) is about
21,000 positions — 160 users at 125 positions each, or fewer at more. Attention over that cache reads
about 3 GB a pass from DDR3, roughly a second at the board's ~2.5 GB/s; the fabric does its arithmetic
in under a second. All of it under the 19 s read.

*Where the 100 MB/s comes from* is the open question. Gigabit Ethernet from a machine holding the file
is the nominal 100 MB/s; the board's own SD slot and USB host are unmeasured; raw NAND on the fabric
headers is a proposal. **Unmeasured factors in this row: the Ethernet streaming rate on this board, the
engine running on the board at all, the DDR3 rate under load, and the processor side's overhead.** Four
factors; the row is arithmetic until each is a number.

What to build: the feeder (processor side reads storage and DMAs weight blocks to the engine), the
cache regions in DDR3 laid out by docs/56's rules, and the same exactness test — **one token with digits
equal to the PC and the Teensy is the gate**, and is what turns two agreeing architectures into a
profile (docs/59 I2).

**This is the rung where serving becomes real.** A module serves a person or a queue; a fabric node
serves a hundred people at once. The module is the personal machine; the fabric node is the village's.

---

## 5. Scale 3 — the bench machine: the inventory, given roles

Everything on the shelf has a job the measurements already assigned:

| part | on hand | role | why (measured) |
|---|---|---|---|
| Zynq 7020 | 2 | crowd servers (§4) | the only device where four-bit unpacking is free (docs/23) |
| Teensy 4.1 | 10 | modules, the split group, and **exact checkers** of the fabric nodes' output | the same digits at 105 s a step (docs/57) |
| Luckfox Pico | 6 | queue front-ends, storage hosts, the network glue | Linux, a card at 17.7 MB/s (docs/33), a USB link at 18.7 MB/s (docs/25) |
| ESP32 (7 S3, ~13 plain, one with a screen) | ~21 | panels, radios, telemetry | keep timed work off them (docs/19: half a millisecond of wifi jitter against an 8 µs window) |

Two fabric nodes and nine modules: about 60,000 tokens an hour to a crowd, about 1.4 million a day, on
about 20 W — **arithmetic stacked on §4's arithmetic**, and stated only so the shape is visible: a
town's worth of answers a day from a shelf drawing what a light bulb draws, *if* four unmeasured factors
each come in. Nothing here is new hardware; it is roles, wiring and the two documents (the profile and
the protocol).

---

## 6. Scale 4 — the shed, and what the module changes about it

docs/36 costed a passively cooled machine holding a 10–20 trillion parameter sparse model: about $300k,
1,503 W against 10,686 W of passive capacity, 2.6 tokens a second to one user, DRAM 12% of the cost, the
DIMM-capable carrier board 60–88% of it and unmeasured. Three things this bench adds to that plan:

1. **The model belongs in flash, not DRAM** (docs/59 §1.5): every terabyte moved off DRAM saves 18.4 W of
   refresh, and a streaming machine reads each weight once a pass, which flash does well. The shed's
   DRAM is for the working memory of its users, not for the model.
2. **Positions are free up to P\***, so the shed's "2.6 tokens a second to one user" is a batch-1 figure
   (docs/59 §1.2, F8); to a crowd it multiplies until the arithmetic catches the read.
3. **The carrier is the module.** docs/36's expensive unknown — a board that puts a processor beside
   addressable memory — is what scale 0 is. The shed is modules with DIMMs and flash instead of PSRAM and
   cards, joined by the protocol of §3. The DDR3 bridge (docs/38–39, simulation only) is the first piece
   of that carrier, and docs/58 Stage 5 (bytes back from a DIMM) is its first measurement.

---

## 7. Scale 5 — the network: what "anywhere" means

Nothing above scale 0 needs a new idea; scale 5 needs none either. A module is $50 and 0.5 W and holds
its model on a card, so:

- **Models travel as objects.** A card is mailed, copied, handed over; its hash proves what it is; it
  runs in twenty years on any board meeting the profile (docs/59 I5). Distribution needs no network and
  cannot be switched off.
- **Groups form by protocol, not by vendor.** Any module that passes the byte-identity test joins a
  split group or a queue; the protocol (docs/59 I4) is a page.
- **Checking is distributed.** Any machine publishing transcripts (docs/59 I2) can be spot-checked by
  any module anywhere: 105 s a step, any step, chosen by the checker. The more modules exist, the more
  steps get checked, and the checkers do not have to trust one another because the answer is a digit
  comparison.

*Label:* proposal, resting on three measured facts (F1–F3) that do not change with the number of
modules.

---

## 8. How this changes AI — the five axes, and the order of moves

### The five things that move

| axis | AI as practised | what this bench has shown, or would |
|---|---|---|
| **where the model lives** | in the RAM of a machine that must hold all of it | in flash, streamed once a pass; RAM only for working memory (measured, scale 0) |
| **who owns it** | rented access to a model on someone else's hardware | the person holding the card and the board (measured: it exists) |
| **whether an answer can be checked** | not by the user: no file, no digits, no reproducibility across runs | any step re-derivable to the digit by anyone with the file and $30 (measured on two architectures; a profile once there are three) |
| **what hardware counts** | current-generation accelerators; last generation discarded | any processor beside any memory, including memory that fails (measured: chips out on half the boots, answers never wrong) and memory that failed testing (proposal, docs/59 I3) |
| **the floor of cost and power** | set by memory bandwidth bought as HBM | $50 and 0.5 W a module; capability bought as flash bandwidth in parallel (measured at one; arithmetic beyond) |

### The order of moves

1. **Publish the proof, every number with its log.** Done and continuing: docs/54–59, `bench-archive/`,
   the public repository, the channel.
2. **Make a stranger's build match.** The wiring sheet (docs/47), the firmware and the PC proof already
   let someone else build a module. The gate is a second bench reporting **identical step lines**. A
   result becomes a technology at the moment someone unaffiliated reproduces it to the digit.
3. **Write the two pages.** The bit-exact inference profile with its conformance suite, and the module
   protocol. Standards are what let a module built by anyone join a group built by anyone else.
4. **Land the three numbers that make people look**, each measured, each with a log:
   (a) the 30-billion-parameter model on the $50 board, exact (L2);
   (b) one person served in under a minute a token by that board (L1 + L5), and in ~15 s by a shelf
   of eight (§3);
   (c) a shelf on a 10 W panel serving a village-day of answers (§3, with the power measured).
5. **The fabric node serves a hundred people.** §4's gate — one exact token on the Zynq — then its
   rate. This is where "a machine for having a model" becomes "a machine for serving one" at village
   scale.
6. **Catch one wrong claim with a $30 board.** Publish a transcript, invite spot-checks, and the first
   time a cheap checker finds a data centre's digit wrong — or confirms a thousand right — the
   asymmetry of docs/59 I2 is demonstrated rather than argued.
7. **Bytes back from a memtest-failed DIMM.** docs/58 Stage 5; the day scrap memory becomes memory.
8. **Ask the model makers for stream-native files** — layouts in pass order, per-tensor widths, block
   checksums (docs/59 I1) — with the yardstick in hand to show what each costs. Models are made for the
   machines that exist; this makes a new kind of machine exist.

Who does each: 1–4 are this bench and its parts; 2 and 6 need strangers, which is what the public
repository and the channel are for; 5 needs the Vivado work already begun in `accel/`; 7 needs the
bridge on hardware; 8 needs 4 to have happened first.

### What it does not change

Training frontier models — this is inference. Interactive chat — a phone is faster and will stay
faster. Energy per token at data-centre scale — this machine should never be sold on it (docs/59 §5).
And the correctness of the model itself: exactness delivers the model's answer, not the right one; the
3B was wrong 5 times in 56 on the PC survey.

---

## 9. Timeline, honestly

| band | what lands | what it needs |
|---|---|---|
| weeks | the PC measurements (α, the 30B's bytes, per-tensor perplexities); the board's power; L1 and L5 on the module | firmware, a USB meter, a USB drive |
| months | L2 on a 32 GB card; two boards byte-identical then eight; the panel; a day's queue log; the two pages written | wiring, the protocol document, a 24-hour run |
| months to a year | one exact token on the Zynq, then its rate; a stranger's identical build | Vivado on the owner's machine; someone else's bench |
| years | the bridge on hardware; a DIMM carrier; the shed's first measured constant | the DDR3 work of docs/38–39 taken to silicon |

The bands are guesses; the gates are not. A rung is climbed when its number is printed by the board
and archived beside the others, and not before.
