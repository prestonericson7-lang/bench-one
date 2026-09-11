# The engineering record

Numbered in the order things were learned, not by topic. That is deliberate: the numbers are a
record of what was believed when, including the places where a later document refutes an earlier one.
Where that happens it is said plainly rather than edited out, because a repository that quietly
rewrites its own history teaches nobody anything.

Gaps in the numbering are real. **15 to 21** were consolidated into 22 and later. **35** was deleted
on purpose: it contained desktop-PC benchmarks, which measure none of the constraints that make this
machine interesting and were actively skewing the conclusions drawn from the rest.

---

## If you are new, read these five

| | |
|---|---|
| [27 — The machine, as specified by measurement](27-the-machine-as-measured.md) | the whole thing in one document, every figure sourced |
| [23 — The real runtime, and the number that changes the architecture](23-real-runtime-measured.md) | where the discovery that read and unpack *add* comes from |
| [33 — A 30B mixture-of-experts, and what it actually costs](33-a-30b-runs.md) | the largest model that fits, and why that class is the only one that does |
| [24 — What this does that a PC cannot](24-what-a-pc-cannot-do.md) | the honest case, and the honest limits |
| [../firmware/bench-one/fpga/ddr3_ice40/](../firmware/bench-one/fpga/ddr3_ice40/) | the DDR3 bridge, standalone and reusable |

---

## Phase one — the heterogeneous stack

How five dissimilar processors and a decoded logic fabric were made to behave as one machine, with
every link speaking one frame.

| | |
|---|---|
| [01 — Brain and radio](01-brain-and-radio.md) | the tested ESP-to-Teensy protocol. **Its summary describes v1 and is wrong**; the correction is in the top-level README |
| [02 — Wiring the stack](02-wiring-the-stack.md) | four-node topology and the exact pinout |
| [03 — The logic fabric](03-logic-fabric.md) | where every IC goes and how it wires |
| [04 — The port map](04-port-map.md) | every connector specified, what backs it, and the bring-up jig |
| [05 — Luckfox setup](05-luckfox-setup.md) | enabling UART3. Nothing works before this |
| [06 — What this stack is fast at](06-performance-and-scaling.md) | what it beats a Pi 5 at, what it does not |
| [07 — Parts and ports: what the research changed](07-parts-and-ports-findings.md) | 74HC is specified nowhere at 3.3 V, and the author's own port design refuted |

## Phase two — the turn towards neural work

| | |
|---|---|
| [08 — The neural array: first numbers](08-neural-array-first-numbers.md) | why weights must live in tightly-coupled memory |
| [09 — What 100 PSRAM chips actually buys](09-scaling-to-billions.md) | storage against bandwidth, and the trillion-weight arithmetic |
| [10 — BUILD SHEET](10-build-sheet.md) | **the wiring plan.** Inventory, topology, power tree, per-node pins, bench order |
| [11 — The interconnect](11-interconnect.md) | how 38 nodes actually talk |
| [12 — The NPU question, settled](12-npu-and-limits.md) | |
| [13 — The dog test](13-vision-result.md) | what happened, honestly |
| [14 — Optimisation: bytes, not instructions](14-optimization.md) | |

## Phase three — measuring it instead of predicting it

This is where the project stops guessing. Most of the architecture follows from 23.

| | |
|---|---|
| [22 — Seven PSRAM chips on one Teensy](22-psram-bank-wiring.md) | the wiring, and the chip-select decode |
| [23 — The real runtime](23-real-runtime-measured.md) | **read and unpack add on a CPU.** The rule everything since is built on |
| [24 — What this does that a PC cannot](24-what-a-pc-cannot-do.md) | |
| [25 — A real model across four nodes](25-distributed-for-real.md) | and the four things that were wrong |
| [26 — Prefill is a different machine from decode](26-prefill-is-a-different-machine.md) | both measured |
| [27 — The machine, as specified by measurement](27-the-machine-as-measured.md) | the consolidated picture |
| [28 — Session log, 2026-09-09](28-session-log-2026-09-09.md) | |
| [29 — A quality yardstick, and the constraint nobody had costed](29-a-quality-yardstick-and-the-kv-cache.md) | perplexity as the judge; the KV cache as the hidden limit |

## Phase four — scaling, and what it would take

| | |
|---|---|
| [30 — Ten paths, and what each is worth](30-ten-paths.md) | |
| [31 — The build, restructured](31-the-build-restructured.md) | |
| [32 — The idle silicon renders itself](32-the-idle-silicon-renders-itself.md) | bit-exact across architectures, so bands stitch seamlessly |
| [33 — A 30B mixture-of-experts runs](33-a-30b-runs.md) | and what it costs the unit |
| [34 — The control plane, and why it is CAN](34-the-control-plane.md) | |
| [36 — The shed](36-the-shed.md) | does $300k hold a 2 to 4 trillion parameter model, passively cooled |
| [37 — Scaling, and what a new chip would have to be](37-scaling-and-new-silicon.md) | |
| [38 — DDR3 on a microcontroller](38-ddr3-on-a-microcontroller.md) | 256 MB on one Teensy, the three ceilings that decide it, and why the limit turns out not to be the DRAM |

## Reference

| | |
|---|---|
| [BOM](BOM.md) | parts, with a ranked buy list |
| [Fabric research](fabric-research.md) | datasheet-level, with sources |
| [The discrete-logic thread](ic-experiments.md) | assessed against the actual box of chips |
| [research/FINDINGS.md](research/FINDINGS.md) | 573 datasheet facts with sources |
| [research/VERIFICATION.md](research/VERIFICATION.md) | the adversarial re-check, **and 29 refuted claims** |

---

## How to read a number in here

Every figure is one of three things and says which:

- **Measured** — taken on the named hardware, with the method stated. Trust it.
- **Calculated** — derived from datasheet figures. Trust the arithmetic, check the premise.
- **Budgeted** — an estimate standing in until someone measures it. Written precisely so that a
  measurement can visibly replace it.

No figure in this directory was taken on a desktop PC. A PC has none of the constraints that make
this hardware interesting, so a PC number is not evidence about the machine. Host builds exist to
check correctness, never speed.
