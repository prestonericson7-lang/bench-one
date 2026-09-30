# The machine, v1 — two FPGA engines, one exact Teensy, two STM32s, one ESP32-P4

Five items on the shelf made into one local AI machine: **two PZ7020-StarLite FPGA boards** as the matrix
engines, **the Teensy 4.1 with its eight PSRAM chips** as the exact reference, **two STM32H743 core
boards** as further exact modules (phase 2) and **the ESP32-P4-NANO** as the front panel (phase 2). The
PC watches over USB consoles and never computes; its Ethernet port stays on the internet. The machine
measures itself and the PC writes the numbers down.

Wiring: nine one-screen pages in [wiring/png/](wiring/png/) (start with `1-in-this-order.png`). The whole
machine is cables and cards; the only wire is the speaker lead on the P4.

## Honest state before bench day

| item | proven | not yet |
|---|---|---|
| Teensy 4.1 + PSRAM | the 3B exact at 105 s a token, two days of logs (docs/57) | the machine's small model on its card |
| FPGA boards | bitstream (GPU + matrix engine + registers + eth1 core) synthesised, timing closed; the engine bit-exact in simulation; the whole Linux image boots in QEMU; the SD card written and hash-verified | **Linux has never reached user space on the board.** The one real boot (2026-09-24) panicked in cpufreq; that is fixed in the image on the card, untested. Every later capture is empty (board unplugged). The engines are the least-proven part of the machine, so FPGA #1's boot report is step one |
| two engines as one | the offload client splits rows across engines; two engines produce the same bytes as one (PC test, this session) | on the boards |
| STM32H743 ×2 | specs recorded | vendor and pins unknown until a silkscreen photo; no firmware yet |
| ESP32-P4-NANO | facts recorded | needs eth1 on an FPGA first; no firmware yet |

## Roles

| item | role | link into the machine |
|---|---|---|
| **FPGA #1** `zynq1` 10.20.0.2 | matrix engine + the machine's host: runs the model on its own processor with both engines taking the rows; runs `machine-bench` | cable 1 to FPGA #2; console to the PC |
| **FPGA #2** `zynq2` 10.20.0.3 | matrix engine | cable 1; console to the PC |
| **Teensy 4.1 + 8 PSRAM** | the exact reference: the 3B as today; later the machine's small model, so every engine answer can be checked to the digit | USB to the PC today; later into an FPGA's USB-A |
| **STM32H743 ×2** (phase 2) | exact modules on a second CPU family: `tl_core` streaming from a TF card, 32 MB SDRAM as the cache ([stm32/README.md](stm32/README.md)) | OTG USB-C into an FPGA's USB-A host port; console USB-C to the PC hub |
| **ESP32-P4-NANO** (phase 2) | the panel: a web page and a chime; later a USB network path for the PC ([p4/README.md](p4/README.md)) | RJ45 to FPGA #2's lower jack once eth1 is on ([zynq/ETH1.md](zynq/ETH1.md)) |

## How they connect

- **Engines' link:** one Ethernet cable, FPGA #1 upper jack ↔ FPGA #2 upper jack. No switch: gigabit
  PHYs cross over by themselves. Both boards run the same card image; a one-line file on each card's FAT
  partition (`zynq-node.txt`: `1` or `2`) gives it its name, address and a fixed MAC before networking
  starts ([zynq/zynq-node](zynq/zynq-node)).
- **The PC:** the two FPGA consoles (lower USB-C, CH340) and the Teensy's USB, on its three ports.
  [bench/machine_watch.py](bench/machine_watch.py) finds them by USB identity and records every line.
  Phase 2 adds a hub for the STM32 and P4 consoles.
- **Second Ethernet jacks (eth1):** physically there, unclocked by the boot files that work. Switched on
  later from U-Boot without touching the first stage — the recipe, the registers and the gate are in
  [zynq/ETH1.md](zynq/ETH1.md). Until then the lower jacks carry nothing.
- **USB-A host ports on the FPGAs:** the STM32s' data link (phase 2), and a USB stick or card reader
  holding a model file (`machine-bench` mounts it and looks for `*.gguf`).

## Cards and power

| card | goes in | holds | written by |
|---|---|---|---|
| 32 GB #1 (in FPGA #1 today) | FPGA #1 | the boot image, `zynq-node.txt` = 1 | already (f94b2434); rewritten with the machine image when ready |
| 32 GB #2 | FPGA #2 | the same image, `zynq-node.txt` = 2 | me, when it is in the PC's reader |
| the Teensy's card | Teensy | `qwen3b.gguf` + `qwen3b.tok` as today; the small model added later | unchanged |
| 4 GB ×2 (phase 2) | STM32 #1, #2 | the small model | me |
| 64 GB / 128 GB | spare; a 32 GB card could carry the 30B for the Teensy's L2 lever later (docs/59) | | |

Power: each FPGA from its own 5 V supply into the **upper** USB-C (J8) with a USB-A → USB-C cable (a
C-to-C cable gives this board no power); the lower USB-C (J2) is the console and powers only the CH340.
The Teensy from the PC's USB. Power-down order: 5 V off J8 first, then the consoles.

## The model plan (labelled)

The engines hold weights resident in their DDR3 (224 MB each, `accel/README.md`), so the model that is
fast is the one that fits them:

| model | on the engines | one user | notes |
|---|---|---|---|
| **Qwen2.5-Coder-0.5B-Instruct, Q8_0 (531 MB)** | int8 rows: 494 MB → ~90% across both engines; int4: 247 MB → all of it, in one engine or split | reading the resident rows once a token at the engine's beat rate (64 bits × 100 MHz ≈ 800 MB/s): 0.3–0.6 s; **arithmetic 1.5–3 tokens a second on one engine, about twice that on two** | the host's own work (norms, attention, the 151,936-way softmax) on a 766 MHz Cortex-A9 is unmeasured and may bound it; int4's quality cost is a `ppl` question on the PC first |
| the 3B (the Teensy's) | does not fit: 1.9 GB against 448 MB | on the Teensy: 105 s a token, exact (measured) | on an FPGA it would stream from its SD like the Teensy does; not the plan |
| Qwen2.5-Coder-1.5B | int4: 770 MB → 58% resident | mixed; later | |

The exact check: `tl_core` accepts Q8_0, so the Teensy runs the same 0.5B file — 531 MB at 24.13 MB/s ≈
22 s plus about 5 s of arithmetic, **~27 s a token, exact** — and any engine answer can be compared step
by step. The engines' math is int8 with a Hadamard rotation (`accel/llm/zaccel_offload.c`), exact in its
integers but not the reference's digits; the comparison measures that gap on hardware, continuously.

**The model file is a download** (huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF, `q8_0`, about
531 MB) and needs the owner's go-ahead; nothing here fetches it. Until it is on a card or a stick, the
bench runs the engine's own bit-exact test and rate (`zaccel-bench`) and the Teensy's 3B.

## Bench day

Page 1 of the wiring is the order. What happens on my side at each step, and the gate that ends it:

| step | I do | gate |
|---|---|---|
| FPGA #1 powered | watch its console: SPL → U-Boot → kernel → login → `ZYNQ-REPORT` | the report prints `pl_done yes`, the three `uio` windows, `zaccel-server active` |
| engine | `machine-bench engine` on FPGA #1 | `zaccel-bench`: every answer bit-exact against the CPU reference, and a rate |
| FPGA #2 powered | same report; `ping zynq2` from zynq1 | both up on cable 1 |
| two engines | `machine-bench engine` again (drives #2's engine from #1), then `model` if a model is present | identical answers; then tokens a second with one engine, with two |
| Teensy | the realistic chat test as today, from the PC | 49 of 49 equal to the PC |
| phase 2 | eth1 on #1, then #2; the P4; the STM32s, one at a time | each its own gate (their READMEs) |

Everything the boards print lands in `bench-archive/<stamp>-machine/` and, from the boards themselves,
in `/boot/reports/` on their cards.

## What is built here (PC), and how it was checked

- **Wiring pages**: [wiring/make_machine_wiring_svg.py](wiring/make_machine_wiring_svg.py) → SVG → nine
  1920×1080 PNGs, same method as the FPGA-GPU wiring.
- **Per-board identity**: [zynq/zynq-node](zynq/zynq-node) + a networkd drop-in; installed by
  [zynq/install.sh](zynq/install.sh), which `mk_sd_image.sh` now calls; the image build writes
  `zynq-node.txt` (`NODE=1|2`).
- **The self-test**: [zynq/machine-bench](zynq/machine-bench) — facts, engine(s), model, Teensy; report to
  console and card.
- **Tools for the Zynq's own CPU**: [zynq/build.sh](zynq/build.sh) (WSL, static armhf, NEON): `run_model`,
  `ppl` with the offload, `tl_ref`, `zaccel-bench`, `test_lib`. Built 2026-09-29, 0.65–0.76 MB each.
- **Two engines as one**: `accel/llm/zaccel_offload.c` rewritten for a host list (`--zaccel h1,h2`): each
  engine calibrated separately, rows divided by measured rate, each engine's free memory capping its own
  slice, one worker per engine, a failed engine retired mid-run while the others carry on.
  `accel/llm/test_two_engines.sh`: text and perplexity with two engines must equal one engine byte for
  byte. **Results 2026-09-29** (`machine/zynq/out_tests.log`, against the real 3B with two CPU-emulated
  engine servers): text identical byte for byte; perplexity 19.749 with one engine, 19.749 with two; an
  engine killed 25 s into a run was reported, retired, and the answer completed. The single-engine
  regression `run_tests.sh` passes on the rewritten client: kernels within 1.15% relative error, identical
  text at half offload, perplexity within 3%, the dying-engine fallback intact.
- **The card image**: built 2026-09-29 by `mk_sd_image.sh` with the machine installed —
  `hardware/pz7020-starlite/linux/out/pz7020-starlite-sd.img.xz` (187 MB; raw 4,226 MiB, sha256
  `02b778bc25c3eb363a18da4b8ec9f0ba93b654b73582207d8675b1acb1f13819`). Its first-stage files — `boot.bin`,
  `u-boot.img`, `zImage`, the DTB, `pl.bit` — are the same bytes card #1 carries today (unchanged since
  2026-09-26). Root partition 4 GB so a model fits beside the system. `zynq-node.txt` = 1 on the image; for
  card #2 the file on its BOOT drive is changed to 2.
- **The PC watcher**: [bench/machine_watch.py](bench/machine_watch.py).
- **eth1**: the recipe in [zynq/ETH1.md](zynq/ETH1.md), not applied.

## What I need from you

1. **A photo of each STM32 board's top and bottom silkscreen**, or the listing / vendor link — the pins.
2. **The go-ahead to download the 0.5B model** (file above, ~531 MB) so it can go on the cards and the
   Teensy; and whether the 1.5B (≈1.1 GB) should come too.
3. **Card #2 in the PC's reader** when convenient: I write it and hand it back.
4. On bench day: the plug-in order on page 1, and nothing else until I ask.
