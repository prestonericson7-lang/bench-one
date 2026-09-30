# Index — everything in this repository, and where to find it

**468 tracked files.** This page exists so you never have to guess which directory something is in.

Every count here is checked against the actual tree by `.claude/verify-index.sh`, which exists because
an index that drifts is worse than no index: it gets believed. It caught two stale counts within
minutes of this file being written. Run it after moving or adding anything.

The build videos are on **[Little Brains Big Mess](https://www.youtube.com/@littlebrainsbigmess)**, which is the fastest way to
see the hardware these files describe actually existing.

If you know what you want, use the table. If you do not, start at
[docs/20 — The actual thesis, with arithmetic](docs/20-the-actual-thesis.md).

---

## Find it by what you are trying to do

| I want to | Go here |
|---|---|
| See a 3-billion-parameter model run on one Teensy, explained for anyone | [docs/57](docs/57-what-we-built-and-what-it-does.md) — every measured number in plain English; then [docs/55](docs/55-teensy-psram-llm-usable-numbers.md) for the numbers and [docs/56](docs/56-one-chip-one-job.md) for the design |
| Know where this goes next, and what has to be proven at each step | [docs/58](docs/58-roadmap-what-this-makes-possible.md) — the roadmap; every line labelled measured, arithmetic or proposal |
| Understand the machine's equations and the levers not yet pulled | [docs/59](docs/59-going-deeper.md) — one token, positions per pass, the node ceiling, splitting across boards; speculative decoding, the 30B, the stall budget, bytes per token |
| See how it is built at each scale, and how it changes AI | [docs/60](docs/60-implementation-at-every-scale.md) — module, shelf, fabric node, bench machine, shed, network: parts, cost, power, output, gates; the five axes and the order of moves |
| Wire the five items into one machine | [machine/README.md](machine/README.md) — the build sheet; [machine/wiring/png/](machine/wiring/png/) — nine one-screen pages, start with 1 |
| See the machine actually work | [docs/40](docs/40-psram-bank-brought-up.md) — 8 MB of PSRAM verified on hardware, and what it cost |
| Read one number correctly | [docs/41](docs/41-the-tuned-settings-were-a-cliff-edge.md) — why the settings in 40 stopped working the same day, and the six rules that came out of it |
| See it do arithmetic | [docs/42](docs/42-matrix-arithmetic-on-the-teensy.md) — 499 MFLOP/s GEMM, 24 Mverts/s, and 234 MMAC/s on 4-bit weights |
| Know which kernel ran | [docs/45](docs/45-the-teensy-had-been-running-the-scalar-kernel.md) — the Teensy is a Cortex-M7 with no NEON, so the headline figure was the scalar fallback; a DSP path is 1.45x and bit-identical |
| Read one day end to end | [docs/46](docs/46-session-log-2026-09-11.md) — the session log: every number measured on 2026-09-11 and every claim of mine it overturned |
| Build the perfboard | [docs/47](docs/47-the-perfboard-build.md) — the wiring sheet, the signal-integrity targets, and what to flash when it is done |
| Know the token rate | [docs/43](docs/43-one-layer-in-tokens-per-second.md) — one token a second, measured end to end, and where the next order of magnitude is |
| Understand the point of the whole project | [docs/20](docs/20-the-actual-thesis.md), then [docs/27](docs/27-the-machine-as-measured.md) |
| Put 256 MB of DDR3 on a microcontroller | [firmware/bench-one/fpga/ddr3_ice40/](firmware/bench-one/fpga/ddr3_ice40/) |
| Wire the DDR3 up tonight | **[WIRING.md](firmware/bench-one/fpga/ddr3_ice40/WIRING.md)** — every wire, both ends, and the part in between. Read [HARDWARE-SAFETY.md](HARDWARE-SAFETY.md) first |
| Flash an FPGA once it arrives | `openFPGALoader -b cu firmware/bench-one/fpga/ddr3_ice40/bitstreams/cfgA.bin` |
| Wire four PSRAM onto a Teensy | the [bank wiring sheet](https://claude.ai/code/artifact/45fa2229-3517-411a-8129-e652d17b0277) — board view, pinouts and all 27 wires — and [docs/22](docs/22-psram-bank-wiring.md) for the seven-chip version |
| Wire the eight-chip perfboard | the [perfboard wiring sheet](https://claude.ai/code/artifact/5f2a27ee-8a0f-429a-8259-c239a69c23f5) — every wire keyed by PSRAM pin number, the one wire that removes the 8 MB cap, and the ground return that has to halve 40 ns to 20.2 — and [docs/47](docs/47-the-perfboard-build.md) |
| See the numbers taken off real boards | [docs/19](docs/19-measured-hardware.md), [docs/23](docs/23-real-runtime-measured.md), [tests/results/](firmware/bench-one/tests/results/) |
| Build and flash something | [firmware/bench-one/tests/](firmware/bench-one/tests/) — one directory per experiment, each self-contained |
| Wire the whole stack | [docs/10 — build sheet](docs/10-build-sheet.md) |
| Know what parts to buy | [docs/BOM.md](docs/BOM.md) |
| Check a claim against a datasheet | [docs/research/FINDINGS.md](docs/research/FINDINGS.md) |
| See what turned out to be wrong | [docs/research/VERIFICATION.md](docs/research/VERIFICATION.md) — 29 refuted claims |
| Read every document in order | [docs/README.md](docs/README.md) |

---

## The six compartments

```
docs/         44 numbered documents + 11 research files    the engineering record
firmware/     350 files                                    everything that runs on a board
hardware/     4 files                                      enclosure and wiring drawings
run/          34 files                                     bench scripts
evidence/     1 file                                       a build log kept as provenance
tools/        not committed                                toolchains, install your own
```

---

## `docs/` — the engineering record

Numbered in the order things were learned, not by topic, so the numbers double as a history of what
was believed when. Full annotated list with phases: **[docs/README.md](docs/README.md)**.

| | |
|---|---|
| **44 numbered documents**, 01 to 47 | the record itself |
| Two gaps | **15 and 16** are absent with no recorded reason. **35** was deleted on purpose, for containing desktop-PC benchmarks that were skewing the conclusions around it |
| 17 to 21 | were misfiled under `firmware/bench-one/docs/` until 2026-09-10, including "Measured hardware" and "The actual thesis" |
| `BOM.md` | parts, with a ranked buy list |
| `fabric-research.md` | datasheet-level research on the glue logic |
| `ic-experiments.md` | the discrete-logic thread, assessed against the actual box of chips |
| `research/` | **11 files.** 573 sourced datasheet facts, plus the adversarial re-check that refuted 29 claims |

---

## `firmware/` — everything that runs on a board

### `bench-one/shared/` — 57 files

Code compiled **identically on every node**, which is the only reason a result from one board means
the same thing on another. If you change something here, every measurement in `docs/` is downstream of
it.

| | |
|---|---|
| `gguf.c` `gguf_bits.c` `gguf_dot.c` | reading quantised model files, and the integer dot product that dominates runtime |
| `model_q.c` | the quantised model runtime, including mixture-of-experts routing |
| `tokenizer.c` | |
| `stage_link.c` | the pipeline link between nodes |
| `machine_scene.h` | the rasteriser, header-only so desktop, Linux board and microcontroller compile the identical file and produce bit-identical output |
| `interop_protocol.h` | byte-identical to the tested original and verified on every build |
| `bench_*.{c,h}` | the earlier hyperdimensional-computing and deep-belief work |
| `bench_pins.h` `bench_ports.h` | the pin and port map as code, so a wiring change is a compile error |

### `bench-one/tests/` — 184 files

One experiment per directory for anything that targets a board; loose `.c` files for host and
cross-compiled programs. **41 experiment directories, 38 C programs, 69 result logs.**

| directory | what it tests |
|---|---|
| **`ddr3_bridge_teensy/`** | **the DDR3 bridge, with the self-calibrating read timing sweep** |
| `ddr3_spd_teensy/` | reads a DIMM's identity EEPROM, unpowered. Step zero of the DDR3 work |
| **`offload_teensy/`** | **does taking work off a Teensy make its memory faster?** Five cases isolating compute, memory, eDMA overlap and the cost of link handling |
| **`usblink_bench/`** | **how fast can a Luckfox feed a Teensy?** The unmeasured number that decides whether the Luckfox is a helper or a bottleneck. Both halves, host side cross-compiled |
| **`psram_driver/`** | **the working PSRAM bank driver.** 8 MB verified over eleven consecutive rounds at 21.3 MB/s write, 13.8 read, on a breadboard, driven by GPIO. Settings corrected in docs/41 |
| **`psram_matrix/`** | **can it compute?** 4x4 vertex transforms, int8 and 4-bit matrix-vector and a float GEMM, in each of the three memory tiers, with checksums that must match across tiers. 499 MFLOP/s and 234 MMAC/s on chip |
| **`psram_layer/`** | **one decoder layer in tokens per second.** Read, compute and both, timed separately, which settles whether the two costs add or overlap: 0.997 of the sum, so they add. Also measures a real Q4_K layer with the shared kernel, so the figure for an actual model is measured rather than scaled -- 0.809 tokens/s at hidden size 256 |
| **`psram_bank6/`** | **8 MB or 48?** Writes a different pattern to every bank before reading any, which detects whether the decoder enable has its own pin yet. Reports the repair when it does not |
| `teensy_pinmap/` | asks the core which GPIO register and bit each pin is, because the pinout card has been wrong here before. Drives nothing |
| `psram_pads/` | sweeps pad drive, slew rate and input hysteresis. All eight configurations land on the same setting and the same 14.53 MB/s, which confirms the wiring as the limit and closes the cheapest route to more speed |
| **`psram_perfboard/`** | **flash this first on the perfboard.** Discovers which of ten chip selects has a chip on it, proves the banks separate, qualifies each one's timing and burst against the refresh limit, verifies the whole flat address space, then soaks every bank indefinitely reporting the error-rate bound |
| `psram_tcem/` | times chip-select-low directly instead of inferring it from throughput. 96 bytes is 6.93 us against an 8 us refresh limit, and 112 is already over |
| `psram_margin/`, `psram_pair/`, `psram_write_margin/` | where the clean timing window begins and ends, one direction at a time and then both together. `psram_margin` also confirms its answer over the whole 8 MB twice and prints the error-rate bound, which is what the first version lacked |
| `psram_addr_fault/`, `psram_pattern/` | is a wrong byte an address line or a timing fault, and does the error rate depend on how hard the data switches |
| `psram_coldstart/`, `psram_settle/`, `psram_idle/` | cold reads against warm ones. `psram_settle` is kept because its control line is the clearest ordering artefact in the tree |
| `psram_soak/` | every candidate configuration, accumulating indefinitely, reporting error rate bounds in parts per billion rather than verdicts |
| `psram_fast/`, `psram_bisect/`, `psram_probe/` | how it got there: the speed sweep, the two-implementations test that found the real bug, and the bit-banged bus probe |
| **`run_host_tests.sh`** | **builds and RUNS every self-verifying test.** Correctness only, never speed. Until this existed none of them linked, so nothing had ever been run |
| `deep_test.c` | the second recall stage, proven without hardware — including that under row sharding it cannot change the answer |
| `psram_bank_teensy/` | the banked PSRAM array and its chip-select decode |
| `psram_teensy/` | one PSRAM chip |
| `can_bus_teensy/` | the control plane |
| `unpack_teensy/` `unpack_esp32/` | the nibble-unpack rate that turns out to be the binding limit |
| `stream_teensy/` `stream_esp32/` | weight streaming |
| `kernel_esp32s3/` | |
| `gfx_teensy/` | the renderer on a microcontroller |
| `sd_teensy/` | storage |
| `system_test_teensy/` | |
| `results/` | **69 logs.** Raw console output behind the figures quoted in `docs/` |

Notable loose programs: `dot_verify.c` (checks the integer dot product against a reference),
`ppl.c` (perplexity, the quality yardstick), `moe_route.c` (expert routing), `kv_attention_bench.c`,
`disk_stream.c`, `machine_view.c` (the renderer), `stage_node.c` and `pipe_model.c` (the distributed
pipeline), `luckfox_bench.c`.

### `bench-one/fpga/` — 23 files

| | |
|---|---|
| **`ddr3_ice40/`** | **the DDR3 bridge.** Standalone, with its own [README](firmware/bench-one/fpga/ddr3_ice40/README.md), test suite and build script. Usable with none of the rest of this project |
| `rtl/` | integer matrix engines for the Zynq boards: `gemv_int4`, `gemm_int4`, `hd_popcount`, `hd_scan` |
| `tb/` | testbenches for those, plus a Python model cross-checking the scan core against the C implementation |
| `run_checks.sh` | simulates, cross-checks and synthesises the Zynq RTL |

### Per-node firmware

| | |
|---|---|
| `teensy1_master/` 14 | the real-time hub. The only bus master |
| `teensy2_worker/` 8 | sliced compute jobs |
| `teensy_hdc/` 9, `teensy_layer/` 3 | the hyperdimensional and layer work |
| `luckfox/` 11 | orchestration on the Linux boards |
| `esp32_radio/` 3, `esp32_worker/` 9, `esp32_isp/` 2 | radio, jobs, and in-system programming |
| `hmi_e32r40t/` 10 | the display node |
| `attiny_guardian/` 1 | |
| `original/` 15 | **the pre-project source, kept untouched.** Everything in `bench-one/` extends this rather than replacing it, and `interop_protocol.h` is verified byte-identical against it on every build |

`firmware/README.md` covers the four phase-one node sketches and the shared protocol. It predates the
neural work and does not describe `tests/`.

---

## `hardware/`, `run/`, `evidence/`

| | |
|---|---|
| `hardware/enclosure/` | the printable tower, as a 3MF, with its own README |
| `hardware/logic-fabric.html` `wiring-the-stack.html` | the wiring drawings, as standalone pages |
| `run/` — 34 files | bench scripts. `run-all.bat`, per-target `test-*.bat`, capture and monitor scripts, and an `ai/` helper set. See `run/README.md` |
| `evidence/build-log.txt` | kept as provenance for a claim in `docs/` |

---

## Top level

| | |
|---|---|
| [README.md](README.md) | what this is, and where to start |
| [INDEX.md](INDEX.md) | this page |
| [HARDWARE-SAFETY.md](HARDWARE-SAFETY.md) | **read before powering any DDR3 work.** Four mistakes destroy a module and one of them looks correct |
| [LICENSE](LICENSE) | MIT |
| **[Little Brains Big Mess](https://www.youtube.com/@littlebrainsbigmess)** | the channel. Build videos for everything here |
| [README-QUESTIONS.md](README-QUESTIONS.md) | phase-one record: specs needed and concerns raised during the first build. Mostly answered since |
| [HANDOFF.md](HANDOFF.md) | phase-one record: the original handoff. **Its protocol summary describes v1 where the firmware is v2**, and a decoder built from it fails on every frame while looking like a wiring fault |
| `.gitignore` | and the reasoning for each rule |

---

## Conventions worth knowing before you read anything

**Every figure is labelled measured, calculated or budgeted**, and the label is load-bearing. A
budgeted number is written precisely so a measurement can visibly replace it.

**No benchmark is run on a desktop PC.** A PC has none of the constraints that make this hardware
interesting. Host builds check correctness, never speed. One document was deleted for breaking this.

**A file ending `_arm` is a build product**, never a source file. Same for `build/`, `*.vvp` and
`*.exe`. None are committed; a clean checkout is about 9 MB.

**Toolchains are not committed.** Install an OSS CAD Suite and point one variable at it:

```bash
OSS_CAD=/path/to/oss-cad-suite sh firmware/bench-one/fpga/ddr3_ice40/build.sh
```

**The index is checked, not trusted.** `sh .claude/verify-index.sh` compares every count in this file
against the tree and every relative link against the filesystem. It exits non-zero when stale.

**And the sketch-local kernel copies.** `python .claude/verify-shared-copies.py` compares all 21 local
copies of `shared/` files across 11 sketches against their masters. Arduino accepts no extra include
path, so sketches that use the real kernels keep copies and their `build.bat` refreshes them — which
only works if the build script is what builds. Calling `arduino-cli compile` directly skips the copy and
measures a stale kernel while reporting it as the shared one (`docs/45`). The checker prints the exact
`cp` to fix each drift.

**The bus layer is checked too.** `python .claude/verify-psram-bus.py` compares the PSRAM bus code --
`put_nib`, `get_nib`, `s_byte`, `addr_out` and both transfer loops -- across all fifteen `psram_*`
sketches, normalising away the no-op counts the sweeps legitimately vary. A no-op count is a timing
specification for one exact instruction sequence, so a sketch that shares a number must share the code.
It exists because an unrolled write loop in the driver, qualified by tests that used a plain one, cost
an afternoon (`docs/41`). A sketch that genuinely needs a different bus layer declares it with a
`/* BUS-VARIANT: reason */` comment.

**Negative results stay in.** 29 refuted claims are in `docs/research/VERIFICATION.md` rather than
deleted, and a document that turned out wrong gets a correction beside it instead of a quiet edit.
