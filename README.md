# BENCH ONE

Running large language models on a shelf of cheap heterogeneous processors — microcontrollers,
small Linux SoCs and little FPGAs — instead of on a GPU. Every figure here is either measured on
hardware or labelled as a calculation. Nothing is rounded up for effect.

The project started as a heterogeneous microcontroller stack with a decoded logic fabric, and that
work is intact and documented. It has since become something more specific: **finding out what this
class of hardware can really do with a model that does not fit in it**, and publishing the numbers
either way.

---

## If you only read one thing

Pick the part you actually want:

| You want to | Go to |
|---|---|
| **Put 256 MB of DDR3 on a microcontroller** | [firmware/bench-one/fpga/ddr3_ice40/](firmware/bench-one/fpga/ddr3_ice40/) — and [docs/38](docs/38-ddr3-on-a-microcontroller.md) for why |
| Understand what this hardware costs and buys | [docs/27-the-machine-as-measured.md](docs/27-the-machine-as-measured.md) |
| See a 30B model actually run | [docs/33-a-30b-runs.md](docs/33-a-30b-runs.md) |
| Wire the stack up | [docs/10-build-sheet.md](docs/10-build-sheet.md) |
| Read every document in order | [docs/README.md](docs/README.md) |
| Build and flash the firmware | [firmware/README.md](firmware/README.md) |

**Read [HARDWARE-SAFETY.md](HARDWARE-SAFETY.md) before powering any of the DDR3 work.** Four specific
mistakes there will destroy a module, and one of them — a divider built with both resistors in series —
looks correct and reads full logic level.

The DDR3 bridge is the piece most likely to be useful on its own. It has no dependency on the rest
of this project: it gives any Teensy 4.1 sixteen times its rated external memory using a scrapped
desktop DIMM and a $30 FPGA board, and it carries its own README, its own test suite and its own
wiring sheet.

---

## The hardware

| Node | Chip | Role in the machine |
|---|---|---|
| Teensy 4.1 ×9 | 600 MHz Cortex-M7 | the compute. Hard real time, 16 MB external RAM each |
| Zynq FPGA ×2 | with onboard DDR3 | matrix engines. Where integer throughput actually lives |
| Luckfox Pico Mini B ×4 | RV1103, Linux | orchestration, storage, tooling |
| ESP32-S3 ×15 | Xtensa LX7 | radio and edge work. Kept off anything timed |
| PSRAM ×60 | 8 MB APS6404 | the weight store, banked off the Teensys |
| 74HC / MCP fabric | glue logic | chip-select decode, IO expansion, analog mux. Not compute |

Fifteen Teensy pins buy 8 SPI chip selects, 128 expander IO, 160 shift outputs, 120 shift inputs and
64 analog channels, from five chips.

---

## What has been measured, and what has not

Measured on hardware:

- **Decode is unpack-bound, not bandwidth-bound.** Nibble unpacking, not memory, is the processor
  limit — 39.3 MB/s on a Teensy, 61 MB/s on a Luckfox. A 31× gap to the FPGAs is what justifies them.
- **Read and unpack costs add on a CPU.** Only fabric overlaps them. This single rule predicts the
  measured PSRAM throughput exactly and is used to forecast every configuration since.
- **A 30B mixture-of-experts model runs**, and its expert cache concentrates 3 to 5× better than
  uniform access would. Streaming experts off board storage is unusable.
- **Distributing costs 0.05%**, but a batch of one wastes every node but the first.
- **Hop cost is 148 µs**, not the 29 µs budgeted, and USB is fast enough that ethernet adapters may
  be unnecessary.
- **4-bit KV cache destroys the model**; int8 is free. Perplexity is the yardstick, not vibes.
- **Rendering is bit-exact across architectures**, so bands computed on different boards stitch with
  no seam.

Not measured, and labelled as such everywhere:

- The DDR3 bridge has never been on hardware. It is simulated, synthesised and datasheet-traced.
- Zynq DDR3 bandwidth is still an estimate.
- NEON unpack rate on ARM is unmeasured.

**Benchmarks are never run on a desktop PC.** A PC has none of the constraints that make this
interesting, so a PC number tells you nothing about the machine. Host builds exist to check
correctness and nothing else.

---

## Repository layout

```
docs/                     the engineering record, numbered in the order it was learned
  research/               sourced datasheet facts, and the adversarial re-check that refuted 29 claims
firmware/
  bench-one/
    fpga/ddr3_ice40/      the DDR3 bridge — standalone, documented, tested
    fpga/rtl/             integer matrix engines for the Zynq
    shared/              code compiled identically on every node, so results are comparable
    tests/               one directory per experiment, host and target
    teensy*/ esp32*/ luckfox/   per-node firmware
hardware/                 schematics and mechanical
run/                      bench scripts
tools/                    toolchains. NOT in the repository; see below
```

`docs/` is numbered chronologically rather than by topic, on purpose: the numbers are a record of
what was believed when, including the places where a later document refutes an earlier one. Where
that happens it is said plainly rather than edited out.

---

## Getting set up

**Toolchains are not committed.** `tools/` holds an OSS CAD Suite install of 16,218 files and is
ignored. Install your own and point `OSS_CAD` at it:

```bash
OSS_CAD=/path/to/oss-cad-suite sh firmware/bench-one/fpga/ddr3_ice40/build.sh
```

For the microcontroller sketches you need `arduino-cli` with the Teensy and ESP32 cores installed.
See [firmware/README.md](firmware/README.md).

A clean checkout is about 9 MB. If yours is 50 MB, the ignore rules are not being applied and you
have compiled output in the tree.

---

## Project norms

These are not aspirations, they are the rules the documents are checked against.

- **Every claim carries its source.** A figure that is budgeted rather than measured says so, and
  the budget is written down precisely so a measurement can visibly replace it.
- **Negative results are published.** 29 claims were refuted by the adversarial re-check and the
  refutations are in the repository, not deleted. A document that was wrong gets a correction beside
  it rather than a quiet edit.
- **Datasheets, not summaries.** A pin number taken from a search result once nearly put 3.3 V on a
  DRAM data line. Pin tables are read from the manufacturer's own document, and the comment beside
  the value names the page.
- **Extend, don't reinvent.** Shared headers are byte-identical across nodes and verified on every
  build, so a result from one board means the same thing on another.
- **Do not invent scope.** A port is specified as a shape with a resource behind it, never as a guess
  at a part.

---

## The honest headline

Not "cheap GPU". This hardware loses general compute badly and will keep losing it.

What it has is two things a GPU cannot buy at any price. The **time domain**: 18.33 ns from a GPIO
edge to an interrupt handler on an M7, against roughly 2 µs with 150 µs spikes under Linux. And
**capacity per dollar at the edge of what is possible** — the constraint that turns out to bound the
whole machine is not compute or bandwidth but **DRAM refresh power**, which is what makes the
enclosure a radiator and sets how large the thing can be before it cooks itself.

That last finding is the most interesting thing in this repository, and it was not what anyone set
out to look for.
