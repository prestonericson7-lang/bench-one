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
| Teensy 4.1 + PSRAM | the 3B exact at 105 s a token, two days of logs (docs/57); the 0.5B proven identical through its core on the PC (bench-archive/20260929-215440); `::model` built into the firmware | flashing that firmware (PC idle, card idle) and copying the 0.5B onto its card (needs the card in a reader) |
| FPGA boards | **On the real board (2026-10-05): SPL, DDR3L, U-Boot, the bitstream (HDMI colour bars + moving square), the kernel, the root filesystem and systemd all ran** — read from card #1's own journal. Bitstream, engine and Linux image as before | The board then stopped between 8.0 and about 13 s of uptime (card #1's ext4 commit times, [cards/fpga1-first-boot-forensics.txt](cards/fpga1-first-boot-forensics.txt)), after Linux gated FCLK0 at 1.91 s (`fclk-enable = <0x00>`): the inferred cause. Fixed three ways (device tree, `clk_ignore_unused`, the `zynq-plcheck` boot guard) and proven in QEMU only. **Card #1 rewritten 2026-10-05 21:35 with image `eeb4afce…`, read back identical; not yet booted.** The next boot decides: a dark-blue screen and `/boot/reports/plcheck.txt` |
| two engines as one | two engines produce the same bytes as one (PC); **emulated on two boards**: both attached across the link, engines bit-exact, the exact reference on the ARM CPU identical to the PC's (that run had the swap export masked) | on the boards, with the PL; the emulated run with the swap export on is being repeated |
| STM32H743 ×2 | specs recorded | vendor and pins unknown until a silkscreen photo; no firmware yet |
| ESP32-P4-NANO | facts recorded | needs eth1 on an FPGA first; no firmware yet |

## Roles

| item | role | link into the machine |
|---|---|---|
| **FPGA #1** `zynq1` 10.20.0.2 | matrix engine + the machine's host: runs the model on its own processor (223 MB of Linux; the model file is mapped, not loaded) with both engines taking the rows, the output head included; runs `machine-bench` | cable 1 to FPGA #2; console to the PC |
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
| 32 GB #1 (in FPGA #1 today) | FPGA #1 | the machine image (same first stage as the card carries now), `zynq-node.txt` = 1 | me, when it is in the PC's reader |
| 32 GB #2 | FPGA #2 | the same image, `zynq-node.txt` = 2 | me, when it is in the PC's reader |
| the Teensy's card | Teensy | `qwen3b.gguf` + `qwen3b.tok` as today, plus `qwen05b.gguf` (staged; copied when the card is in a reader); `::model qwen05b.gguf` switches | me, file copy |
| 4 GB ×2 (phase 2) | STM32 #1, #2 | `qwen05b.gguf` (staged) | me, file copy |
| 64 GB / 128 GB | spare; a 32 GB card could carry the 30B for the Teensy's L2 lever later (docs/59) | | |

Power: each FPGA from its own 5 V supply into the **upper** USB-C (J8) with a USB-A → USB-C cable (a
C-to-C cable gives this board no power); the lower USB-C (J2) is the console and powers only the CH340.
The Teensy from the PC's USB. Power-down order: 5 V off J8 first, then the consoles.

## The model plan (labelled)

The engines hold weights resident in their DDR3 (224 MB each, `accel/README.md`), so the model that is
fast is the one that fits them:

| model | on the engines | one user | notes |
|---|---|---|---|
| **Qwen2.5-Coder-0.5B-Instruct, Q8_0 (676 MB on disk; 494 M weights)** | int8 rows: 494 MB → ~90% across both engines; int4: 247 MB → all of it, in one engine or split | reading the resident rows once a token at the engine's beat rate (64 bits × 100 MHz ≈ 800 MB/s): 0.3–0.6 s; **arithmetic 1.5–3 tokens a second on one engine, about twice that on two** | the host's own work (norms, attention, the 151,936-way softmax) on a 766 MHz Cortex-A9 is unmeasured and may bound it; int4's quality cost is a `ppl` question on the PC first |
| the 3B (the Teensy's) | does not fit: 1.9 GB against 448 MB | on the Teensy: 105 s a token, exact (measured) | on an FPGA it would stream from its SD like the Teensy does; not the plan |
| Qwen2.5-Coder-1.5B | int4: 770 MB → 58% resident | mixed; later | |

The exact check: `tl_core` accepts Q8_0, so the Teensy runs the same 0.5B file — 676 MB at 24.13 MB/s ≈
28 s plus about 5 s of arithmetic, **~33 s a token, exact** — and any engine answer can be compared step
by step. The engines' math is int8 with a Hadamard rotation (`accel/llm/zaccel_offload.c`), exact in its
integers but not the reference's digits; the comparison measures that gap on hardware, continuously.

**The model files are on the image and staged for every card** ([cards/README.md](cards/README.md)):
`qwen05b.gguf` = Qwen2.5-Coder-0.5B-Instruct `q8_0` from huggingface.co/Qwen (675,710,848 bytes, sha256
`e1a77721…` verified against the repository's own checksum, downloaded 2026-09-29), and `qwen3b.gguf` =
the Teensy's 3B, so the Zynq's CPU can run `tl_ref` on the same file and its digits can be compared with
the Teensy's — a third architecture for the exactness profile (docs/59 I2). The 0.5B through the exact
core on the PC: **ALL IDENTICAL** (tokenizer on 30,338 lines + 2,000 fuzz records; forward per token,
batched, together; M7-emulated kernels and forward). Its quality through the engines' int8/int4 path is
measured by `ppl` on the boards, not assumed.

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
- **Per-board identity**: [zynq/zynq-node](zynq/zynq-node) run by `zynq-node.service`, an early one-shot
  before `network-pre.target` (a first version as networkd's `ExecStartPre` changed nothing, because that
  unit's sandbox makes `/etc` read-only — caught by the QEMU boot below); installed by
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
- **The card image**: rebuilt 2026-10-05 by `mk_sd_image.sh` with the machine, both models, the Pi's
  swap export and the PL guard — `hardware/pz7020-starlite/linux/out/pz7020-starlite-sd.img.xz` (raw
  4,431,282,176 bytes, sha256 `eeb4afcec81cfa27e0233c94d1e58f21022683563321751b74d9ba8c28926c29`).
  Against what card #1 ran on 2026-10-05, the boot partition differs in exactly two files: the device
  tree (FCLK0 held) and `boot.scr` (`clk_ignore_unused`). `boot.bin`, `u-boot.img`, `zImage` and `pl.bit`
  are byte for byte the ones that ran on the board, so the first stage is unchanged. Root partition 4 GB:
  the system plus `/opt/machine/models`. `zynq-node.txt` = 1 on the image; for card #2 the file on its
  BOOT drive is changed to 2.
- **The PC watcher**: [bench/machine_watch.py](bench/machine_watch.py).
- **eth1**: the recipe in [zynq/ETH1.md](zynq/ETH1.md), not applied.
- **The Teensy's `::model NAME.gguf`** (`firmware/bench-one/tests/psram_llm/psram_llm_board.inc`): records
  the name in `model.txt` on the card and restarts the board with the card idle; the board opens that file
  and builds its tokenizer store beside it; the `I` line names the model. Compiled 2026-09-29 (flash
  178 KB, RAM1 38.9 KB free for locals; the hex is in the sketch's build output, not flashed — a flash
  needs the PC idle and the board between runs, bench day).
- **The 0.5B through the exact core** (`verify_pc.py`, now `MODEL=` from the environment): all sections
  identical, `bench-archive/20260929-215440-psram_llm-pcverify/`.
- **The cards** ([cards/](cards/)): what goes on each and `stage.py`, which gathers the image, the node
  file and the model into `D:\start\machine-cards\<card>\` with checksums so each card is one step in the
  reader.
- **Two engines as one, plus the output head** (`accel/llm/zaccel_offload.c`, second pass): the offload
  now covers the output head too — 28% of the 0.5B's multiply-adds and 10% of the 3B's were staying on
  the host CPU — which needed **row bands**, because the engine's row count is 16 bits and the head has
  151,936 rows; bands of 16,384 rows are also the unit of quantisation, so an upload's working memory
  stays under ~50 MB on the board's 256 MB. `$ZACCEL_HEAD=0` keeps the head on the CPU. On the 3B (PC):
  identical text at half offload with the head, perplexity 19.349 → 19.791 (half) / 19.436 (all),
  within 3%; two engines still byte-identical to one.
- **The model is mapped, not read** (`shared/gguf.c`, `model_q.c`): the runtime used to `malloc` and
  `fread` every tensor — 676 MB for the 0.5B on a board whose Linux has 223 MB. Found by the two-board
  emulation: `tl_ref` and `run_model` OOM-killed with 15 MB free. On POSIX the file is now mapped
  read-only and tensors point into the map; the page cache streams what is used, the way the Teensy
  streams its card. Windows keeps the read path. The Pi is unaffected except for using less RAM.
- **The swap export was masked, then restored (2026-10-05)**: `zynqram`, a 128 MB tmpfs file exported to
  the Orange Pi as swap, takes 128 MB of the board's 223 MB whether or not a Pi is attached. It was
  masked for the two-board run below. It is back because card #1 also serves the Pi, and that export
  is the Pi's swap. `zynq/install.sh` now removes the mask if an older image left one. Measured in QEMU
  with it on: 57 MB available of 223 (149 MB with it masked).
- **Two boards emulated** (`zynq/qemu_two_node_test.sh`): two QEMU `xilinx-zynq-a9` machines from the
  card image, node 1 and node 2, joined by a socket link in place of cable 1; node 1 runs the bench-day
  command `machine-bench all` against the 0.5B — facts, its engine, the peer's engine over the link,
  `tl_ref`, the fused path, one engine, both engines, perplexity — and the exact reference's lines are
  pulled off the card for a byte comparison with the PC. **Result 2026-09-30**
  (`zynq/qemu-two-node/`): both nodes up as `zynq1` / `zynq2`; ping across the link; `machine-bench`
  sees the peer; the engine on each node **bit-exact** (74 answers checked, 0 wrong — the server's CPU
  fallback, QEMU having no PL); the peer's engine driven from node 1; **`tl_ref` on the ARM Cortex-A9:
  12 step lines byte-identical to the PC's x86 `tl_ref`** on the 0.5B (`tl_ref-qwen05b-arm.txt` against
  `-pc.txt`) — the third architecture of the exactness profile, in emulation; `run_model --fast` produced
  tokens; one engine attached by measured split (12.9%); **both engines attached across the link** (5.2%;
  under emulation the "engine" is the same slow CPU, so the measured split rightly keeps most rows local
  — on the boards the PL's rate decides). Free memory during the run: 149 MB (15 MB before the swap
  export went). Timings under TCG (≈100 s a token) are not numbers. The perplexity step outlasted the
  80-minute window on the first run; **the record run with emulation-sized steps: PASS on all 11
  checks** (`qemu-two-node/checks.txt`, `machine-bench-zynq1-record.txt`): `tl_ref` again identical to
  the PC line for line; perplexity on the 0.5B over 8 scored tokens 6.209 on the CPU alone, 6.221 with
  12.9% of the rows (head included) on the engine, top-1 agreement 85.7% both — the real armhf binaries,
  a real quality figure, a short sample.
- **The image in QEMU** (`zynq/qemu_machine_test.sh`): boots the machine image on `xilinx-zynq-a9` twice,
  with `zynq-node.txt` = 1 and = 2, and checks the hostname, the address, the two models, that the static
  tools execute on the card's glibc, and that `machine-bench` runs and writes its report to the card.
  **Result 2026-09-29: PASS on all 13 checks** (`machine/zynq/out_qemu.log`): `zynq-node: node 1 ->
  zynq1 10.20.0.2` and `node 2 -> zynq2 10.20.0.3` in the kernel log, the login banner and prompt
  `zynq1` / `zynq2`, both models listed, the static `run_model` prints its usage on the board's glibc,
  `machine-bench facts` runs and its report lands in `/boot/reports/`. The first version of the identity
  step failed this test silently (networkd's sandbox) and was replaced — which is what the test is for.
- **The PL guard** (`hardware/pz7020-starlite/linux/zynq-plcheck`, after the first real boot stopped on
  2026-10-05): before anything reads the PL it checks FCLK0's gate bit, the PS/PL level shifters and the PL
  reset, syncs every step to `/boot/reports/plcheck.txt`, and holds fpgagpud, zaccel-server and zynq-agent
  back if a read would hang the bus. `qemu_plcheck_test.sh` boots through the production U-Boot twice:
  card #1's own `boot.scr` and device tree, then today's. QEMU kept the gate bit Linux wrote with card
  #1's device tree (`FPGA0_THR_CNT = 1`, fclk0 enable count 0). The guard named FCLK0 as the only problem,
  read nothing, held both services back, and the board stayed up. With today's files the bit is clear,
  the count is 1, the reads are attempted and the verdict is OK. **PASS on all 19 checks.**

## What I need from you

0. **Card #1 back in FPGA #1 and the Pi's card back in the Pi**, cabled and powered as on 2026-10-05.
   Both are rewritten and verified. If Windows offers to format either card when you pull it, answer No.
   Then watch FPGA #1's HDMI. A dark-blue screen means the GPU daemon is running and the fix worked;
   colour bars that stay mean it did not. Either way, bring card #1 back to the reader afterwards:
   `/boot/reports/plcheck.txt` on it says how far the board got. On the Pi, if the rest of the NVMe drive
   is unpartitioned (its kernel log shows one partition), that space appears at `/mnt/nvme`; the old
   256 MB FAT partition is left as it is. Either way the Pi records what it found in
   `/var/lib/accel/nvme-auto.log` on its card.
1. **A photo of each STM32 board's top and bottom silkscreen**, or the listing / vendor link — the pins.
   Nothing else about those boards is known well enough to write firmware against.
2. **Cards in the PC's reader**, one at a time, whenever convenient: card #2 (I write the image and the
   node file), the Teensy's card (I copy the 0.5B onto it), the two 4 GB cards (the same file). Each is a
   minute; I say when it is done.
3. On bench day: the plug-in order on page 1, and nothing else until I ask.
