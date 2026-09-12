# 53 — Handoff: everything measured, and where it stands

*Written 2026-09-12. Every number here came off the hardware unless marked. This is the document to
read first if you are picking this project up — from me, or from anyone.*

## What this machine is

A bookshelf-sized distributed local-AI machine built from cheap heterogeneous processors, FPGAs and
RAM instead of a GPU. The thesis is capacity per dollar: PSRAM is the reserve, and the design question
is how to compute against memory that is enormous and slow.

## The headline numbers, in the order they were learned

| stage | MMAC/s | what changed |
|---|---|---|
| starting point | 14.76 | **not real** — throughput over six banks of garbage data |
| honest baseline | 8.4 | same hardware, only verified bytes counted |
| one bit a weight | 31.1 | 8 weights a byte instead of 2 |
| batching | 187.1 | weights read once, scored 16 times |
| Luckfox computing alongside | 401.0 | the idle board put to work |
| batch 64, full capacity | 316.7 | 587 M parameters in one pass |

The Luckfox alone, on its own DDR2: **648 MMAC/s at 81 MB/s** — 21× the entire eight-chip PSRAM array.

## Measured hardware facts

| thing | value | how it was established |
|---|---|---|
| Luckfox DDR2 + NEON, 1-bit | 81.0 MB/s, 648 MMAC/s | NEON kernel vs scalar reference, exact match |
| Teensy quad bank (CS0/CS1) | 10.4–11.7 MB/s | read-back verified over 1 MB |
| Teensy single-bit bank | 3.2–3.7 MB/s | same |
| Teensy 8-chip aggregate | 3.9 MB/s | all banks, sequential |
| real tCEM (chip-select low) | **29.9 µs clean**, datasheet says 8 | filled once, read back 8× per burst, 8 MB a setting |
| PSRAM identity | 0x0D 0x5D | distinguishes a chip from an open bus |
| quad read wait cycles | 6, and only 6 | swept 4–96; nothing else works on any bank |
| Teensy at 816 MHz | stable, +13–20% | measured with a clean port; cooling fine at 38 °C |
| Luckfox governor | was `ondemand`, dropping to 408 MHz | pinned to `performance`, passes within 0.4% |

## The three quantisation kernels, all verified bit-identical

All cost the same **1.25 instructions per weight**, so at a bus-limited rate each halving of the weight
doubles the multiply-accumulate rate.

| bits | weights/byte | extraction |
|---|---|---|
| 4 | 2 | mask `0x0F0F0F0F`, `SXTB16`, `SMLAD` |
| 2 | 4 | lane j = `(v >> 2j) & 0x03030303`, zero point 2 |
| 1 | 8 | lane j = `(v >> j) & 0x01010101`, bit means −1 or +1 |

Every one is checked against arithmetic the Luckfox computes in Python on its own core before any rate
is reported. Integer arithmetic, so a correct kernel is bit-identical and there is no tolerance to argue
about. The ESP32-S3 uses a different kernel — a 128 kB table of every possible weight byte, one load and
one add per 8 weights — which is exact, not approximate, and verifies against the same reference.

## Six banks that read wrong, and what was ruled out

The six external chips take a quad **write** perfectly and return wrong nibbles to a quad **read**.
Ruled out by measurement, not by argument:

| cause | test | result |
|---|---|---|
| chip absent | 250 kHz identity probe | all six answer 0D 5D |
| shared chip select | per-bank tag, read each against every tag | no off-diagonal match |
| the write direction | single-bit read-back of quad-written data | correct at four addresses |
| one broken data wire | wrong bits per line over 32,768 nibbles | spread evenly, ranking shuffles |
| clock speed | 10.7 MHz down to 2.3 MHz | no improvement |
| burst length | 8 to 64 bytes | shorter is better, never clean |
| wait cycles | 4 to 96 | 6 is right; nothing helps the others |
| sample instant | 7 positions | no effect, the window is wide |
| bus keeper / hysteresis / drive / pull-up | four pad configs, **correct bit positions** | no effect |

Every one of those chips reads correctly when it drives **one** line instead of four. That fallback is
what recovers 48 MB.

**Unresolved:** Y1 and Y3 have since returned 24 bytes of clean quad data, and the failure mode is a
chip that stops driving partway through a burst and recovers at the next boundary. That is not a dead
path and it is the biggest single prize left — quad on those six is 2.6× on 75% of the memory.

## Mistakes that cost real time — read these before repeating them

1. **An orphaned process holding a serial port** caused garbled identity strings, replies welded
   together, "device reports readiness to read but returned no data", and what looked for an hour like a
   hardware fault at 816 MHz. Every serial port here tolerates exactly one reader. Check
   `/proc/*/fd` before blaming hardware.
2. **Template instantiations retime the bus.** Adding the 0x0B read path cost the quad banks 21% on an
   unchanged setting: `bread<8>` is 0x29C bytes in the fast build and 0x244 in the slow one, from
   identical source, because GCC stops unrolling when the file grows. Check the `.sym` size after any
   edit.
3. **A benchmark that does not check its own answer is not a benchmark.** The partial sum drifted one
   round in five for the whole life of the project because nothing compared consecutive rounds.
4. **A sweep is not a qualification.** 32 kB of silence bounds the error rate below ~1 in 30,000; the
   fault that bit this project ran at 1 in 600,000.
5. **Hand-assembled register fields.** The pad-control constants had SPEED at the wrong offset, so every
   "improved" value silently set the slowest pad bandwidth and the conclusion drawn from that sweep was
   worthless. Read the header.
6. **`(void)total` let -O3 delete the whole benchmark.** The local side reported 0.03 s for work that
   cannot take under 1.6 s. Accumulators must be observable.
7. **The host must own every knob, every time.** A diagnostic left the quad wait count at 96 and the next
   benchmark silently failed both quad banks into single-bit mode.

## The architecture, settled

- **The Teensy does the arithmetic and nothing else.** No discovery, no timing decisions, no soak, no
  logging. It answers commands and returns numbers. A worker holding policy is a worker that drifts.
- **The host owns every decision.** Which selects exist, bus mode, timing, burst, allocation, logging.
  Changing a policy means editing a file, not reflashing a microcontroller.
- **Weights never cross the wire.** The host sends the *rule* that generates them; each node materialises
  them at its own memory speed. A megabyte at 1 Mbaud is eight seconds.
- **Every node computes at once**, so a pass costs the slowest node, not the sum. Therefore a node holds
  bytes in proportion to how fast it reads them, not how many it can hold.

## The stack as designed (not yet built)

19 addressable nodes on one self-numbering chain. One firmware image per board type; nothing configured
at flash time.

| tier | count | capacity | MB/s |
|---|---|---|---|
| Luckfox DDR2 + NEON | 5 | 14 MB each | 81.0 measured |
| Teensy, 2 chips | 8 | 16 MB each | ~11 measured |
| Teensy, 8 chips (head) | 1 | 64 MB | 3.9 measured |
| ESP32-S3 | 6 | 8 MB each | **unmeasured** |

Total 310 MB = 2.48 billion parameters at one bit. Spread by bandwidth rather than evenly,
~1.68 B parameters run at a second a token *(the ESP32 term in that projection is a guess)*.

Wiring: [docs/52](52-one-system.md), and the pin-by-pin list of all 57 wires is in the published
diagram.

## Code, and what has actually run

| file | state |
|---|---|
| `tests/psram_worker/psram_worker.ino` | **runs.** Single node proven. Chain additions compiled, never run. |
| `tests/esp32s3_node/esp32s3_node.ino` | compiled, never run |
| `luckfox/drive.py` | **runs.** Single-node qualification and benchmark, hardened. |
| `luckfox/hybrid.c` | **runs.** One Teensy + local NEON, concurrent, 1.30× measured. |
| `luckfox/neon1bit.c` | **runs.** 648 MMAC/s measured. |
| `luckfox/leafd.c` | compiled, never run |
| `luckfox/fleet.py` | written, syntax-checked, never run |
| `tools/bench_run.py` | **runs.** Archives every flash and log before it flashes. |

**Nothing involving the chain has touched hardware.** Enumeration, addressing, relaying, `leafd` and
`fleet.py` are all unproven. First contact will find bugs.

## Where to pick it up

1. **Flash and prove one node with the chain firmware.** `A 1` should return `A 1 here leaf 0`, and
   `@1 I` should answer. That validates addressing without any wiring.
2. **Two nodes.** Proves relaying and that the non-blocking pump does not drop bytes.
3. **One leaf.** Proves `leafd` and the Serial3 port.
4. **Then `fleet.py`.** It has never run; expect to debug the enumeration timeout and the reply matching
   first.
5. **The quad-read fault on the six external chips** is the largest performance prize still open.

## Hard rules this project runs by

- **Log all data and save the flash between each test.** `tools/bench_run.py` refuses to flash anything
  it cannot archive first. Every run's exact image and full serial log is in `bench-archive/`.
- **No guessing.** Read the header, grep the file, list who holds the device, re-read the docs — before
  forming a hypothesis. Every time that rule was broken here it cost hours.
- **Never benchmark on the desktop.** Host builds are for correctness only.
- **Nothing that risks the hardware.** Software risk is cheap because the archive exists; physical risk
  is not on the table.
