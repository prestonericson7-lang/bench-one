# Index — everything in this repository, and where to find it

**439 tracked files.** This page exists so you never have to guess which directory something is in.

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
| See the machine actually work | [docs/40](docs/40-psram-bank-brought-up.md) — 8 MB of PSRAM verified on hardware, and what it cost |
| Understand the point of the whole project | [docs/20](docs/20-the-actual-thesis.md), then [docs/27](docs/27-the-machine-as-measured.md) |
| Put 256 MB of DDR3 on a microcontroller | [firmware/bench-one/fpga/ddr3_ice40/](firmware/bench-one/fpga/ddr3_ice40/) |
| Wire the DDR3 up tonight | **[WIRING.md](firmware/bench-one/fpga/ddr3_ice40/WIRING.md)** — every wire, both ends, and the part in between. Read [HARDWARE-SAFETY.md](HARDWARE-SAFETY.md) first |
| Flash an FPGA once it arrives | `openFPGALoader -b cu firmware/bench-one/fpga/ddr3_ice40/bitstreams/cfgA.bin` |
| Wire four PSRAM onto a Teensy | the [bank wiring sheet](https://claude.ai/code/artifact/45fa2229-3517-411a-8129-e652d17b0277) — board view, pinouts and all 27 wires — and [docs/22](docs/22-psram-bank-wiring.md) for the seven-chip version |
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
docs/         37 numbered documents + 11 research files    the engineering record
firmware/     331 files                                    everything that runs on a board
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
| **37 numbered documents**, 01 to 40 | the record itself |
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

### `bench-one/tests/` — 165 files

One experiment per directory for anything that targets a board; loose `.c` files for host and
cross-compiled programs. **25 experiment directories, 36 C programs, 69 result logs.**

| directory | what it tests |
|---|---|
| **`ddr3_bridge_teensy/`** | **the DDR3 bridge, with the self-calibrating read timing sweep** |
| `ddr3_spd_teensy/` | reads a DIMM's identity EEPROM, unpowered. Step zero of the DDR3 work |
| **`offload_teensy/`** | **does taking work off a Teensy make its memory faster?** Five cases isolating compute, memory, eDMA overlap and the cost of link handling |
| **`usblink_bench/`** | **how fast can a Luckfox feed a Teensy?** The unmeasured number that decides whether the Luckfox is a helper or a bottleneck. Both halves, host side cross-compiled |
| **`psram_driver/`** | **the working PSRAM bank driver.** 8 MB verified at 23.7 MB/s write, 14.8 read, on a breadboard, driven by GPIO |
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

**Negative results stay in.** 29 refuted claims are in `docs/research/VERIFICATION.md` rather than
deleted, and a document that turned out wrong gets a correction beside it instead of a quiet edit.
