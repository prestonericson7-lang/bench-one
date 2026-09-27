# BENCH ONE

Running large language models on a shelf of cheap heterogeneous processors — microcontrollers,
small Linux SoCs and little FPGAs — instead of on a GPU. Every figure here is either measured on
hardware or labelled as a calculation. Nothing is rounded up for effect.

Built and documented on **[Little Brains Big Mess](https://www.youtube.com/@littlebrainsbigmess)** — the builds, the failures and the
bench footage behind every number in this repository.

The project started as a heterogeneous microcontroller stack with a decoded logic fabric, and that
work is intact and documented. It has since become something more specific: **finding out what this
class of hardware can really do with a model that does not fit in it**, and publishing the numbers
either way.

---

## Accelerators for the Orange Pi — 2026-09-25, re-verified 2026-09-26

**[accel/README.md](accel/README.md)**: the Zynq and the Teensy as the Orange Pi 4 Pro's accelerators.
One PL bitstream carries the FPGA-GPU (HDMI out, Teensy 4.1 as its geometry engine), a batched
int4/int8 matrix engine fed by DMA from the Zynq's DDR3, and the platform; 128 MB of the Zynq's
512 MB (one x16 DDR3L chip -- not the 1 GB some listings claim) is the Pi's swap over the network.
Built, simulated, timing-closed, booted to Linux in QEMU at the board's real clocks, and on both SD
cards; the Pi's first real boot was read back off its card and the two faults it showed are fixed.
The board measurements are the next step.

### Build it yourself

Everything needed is here. The scripts were written on one Windows 11 PC with WSL2 Ubuntu 22.04 and
carry its paths (the repo at `/mnt/d/espicpc`, Vivado at `D:\2026.1`, vendor images under Downloads),
mostly as overridable defaults near the top of each script -- set those for your machine.

| Part | Start here |
|---|---|
| Wiring (one screen per page) | [hardware/pz7020-starlite/wiring-png/](hardware/pz7020-starlite/wiring-png/), table in [accel/gpu/WIRING.md](accel/gpu/WIRING.md) |
| Zynq SD card (U-Boot SPL + Linux + PL bitstream) | [hardware/pz7020-starlite/PS-LINUX.md](hardware/pz7020-starlite/PS-LINUX.md), [BOOT-SD-runbook.md](hardware/pz7020-starlite/BOOT-SD-runbook.md) |
| PL design (Vivado 2026.1) | [hardware/pz7020-starlite/VIVADO.md](hardware/pz7020-starlite/VIVADO.md) |
| Orange Pi card (official image + self-installing bundle) | [accel/README.md](accel/README.md), `accel/build_pi_card_image.sh` |
| Every check, one verdict | `bash deploy/run_all_tests.sh` (47 checks: RTL testbenches, host tests, QEMU boots of both systems' software) |

Hardware: Puzhi PZ7020-StarLite (XC7Z020), Orange Pi 4 Pro, Teensy 4.1. Board facts are sourced to
the vendors' manuals and schematics page by page; the traps that cost a boot are written down where
they bit (`hardware/pz7020-starlite/PS-CONFIG.md`, `accel/README.md`).

## Current state — 2026-09-12

**[docs/53 — Handoff](docs/53-handoff.md)** is the document to read first. Every measurement, every
mistake worth not repeating, and exactly which code has run on hardware and which has not.

The short version, all measured:

| | |
|---|---|
| best rate | **401 MMAC/s** with a Teensy and a Luckfox computing concurrently |
| one node, batch 64 | 316.7 MMAC/s over 587 million parameters |
| Luckfox alone, NEON, 1-bit | 648 MMAC/s at 81 MB/s — 21x the whole PSRAM array |
| quantisation | 1 bit a weight is 3.69x the 4-bit rate, verified bit-identical |
| real tCEM | 29.9 us clean, against a datasheet figure of 8 |

A 19-node self-numbering chain is designed and built in firmware — nine Teensys, five Luckfoxes, six
ESP32-S3s, 310 MB, 2.48 billion parameters at one bit. **None of the chain code has run on hardware
yet.** docs/53 says precisely what has and what has not.

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
| **Find anything at all** | **[INDEX.md](INDEX.md)** — every file and directory, catalogued |
| Build and flash the phase-one sketches | [firmware/README.md](firmware/README.md) |

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

For the microcontroller sketches you need `arduino-cli` with the Teensy and ESP32 cores installed:

```bash
arduino-cli compile --fqbn teensy:avr:teensy41 firmware/bench-one/tests/ddr3_bridge_teensy
```

Each directory under `firmware/bench-one/tests/` is one self-contained experiment and compiles on its
own. [firmware/README.md](firmware/README.md) covers the four phase-one node sketches and the shared
protocol header; it predates the neural work and does not describe the tests.

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

---

## License

MIT for everything written for this project ([LICENSE](LICENSE)) -- use it, change it, build it, sell
it. A few files come from elsewhere (AMD's generated Zynq init, the osmocom RTL-SDR tuner driver,
Ubuntu packages and a Linux module shipped for offline installs) and keep their own licences; they
are listed with their sources in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
