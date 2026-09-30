# 61 — The machine: the five items made into one

2026-09-29. The owner's ask, in his words: the Teensy with all its PSRAM, the two FPGAs, the two STM32s and
the ESP32-P4 "need them to work together to be a huge math matrix engine for a local AI"; he wires and
plugs in, the rest is mine, then he plugs it into the PC and I benchmark it.

The build sheet, the roles, the network, the cards, the model plan, the bench-day gates and the honest
state of every item live in **[machine/README.md](../machine/README.md)**. The wiring is nine one-screen
pages in [machine/wiring/png/](../machine/wiring/png/). This page records only what changed in the
repository and why.

## What the machine is

Two PZ7020 FPGA boards are the matrix engines; they link to each other on their own gigabit ports
(no switch, no PC port), boot the same card image, and take their name and address from one file on the
card. FPGA #1 is also the host: the model runs on its own processor with both engines taking the matrix
rows. The Teensy is the exact reference. The STM32s become exact modules and the P4 the panel in a
second phase, each behind its own gate. The PC listens to consoles.

## What changed in the repository

- `accel/llm/zaccel_offload.c`: **several engines**. `--zaccel h1,h2` calibrates each engine, divides the
  offloaded rows by measured rate, caps each slice by that engine's free memory, drives one worker per
  engine, and retires a failed engine mid-run without stopping the others. Every row is still computed
  whole by one engine, so two engines return the same bytes as one — `accel/llm/test_two_engines.sh`
  holds it to that, beside the existing `run_tests.sh`.
- `machine/zynq/`: `zynq-node` (identity before networking), `machine-bench` (the self-test),
  `build.sh` (the model runner, reference and engine tools for the Zynq's own Cortex-A9), `install.sh`
  (called from `mk_sd_image.sh`), `ETH1.md` (the second jack, from U-Boot, first stage untouched).
- `machine/wiring/`: the drawing and its pages. `machine/bench/machine_watch.py`: the PC's console
  recorder. `machine/stm32/`, `machine/p4/`: the phase-2 plans with what each needs before firmware.

## The cards are ready before they are plugged in

The image carries both models (`/opt/machine/models/qwen3b.gguf`, the Teensy's 3B; `qwen05b.gguf`, the
0.5B Q8_0 downloaded and checksum-verified 2026-09-29) on a 4 GB root, with the first-stage files byte
for byte those card #1 already boots. The 0.5B ran through the exact core on the PC: all sections
identical (`bench-archive/20260929-215440-psram_llm-pcverify`). The Teensy firmware gained `::model
NAME.gguf` and compiles. `machine/cards/` says what goes on every card and stages it with checksums.
`machine/zynq/qemu_machine_test.sh` boots the image in QEMU as node 1 and node 2 and checks the
identity, the models, the tools and the self-test on the real root filesystem.

## Two boards emulated, and what that found

`machine/zynq/qemu_two_node_test.sh` boots two QEMU Zynqs from the image on a socket link and has node 1
run the bench-day command against the 0.5B. The first runs found, in order: a console driver whose
sentinel matched an earlier command's; the engines bit-exact on both nodes and the peer driven over the
link; then every model step killed by the OOM killer — the runtime read the whole model into RAM, and
the board's 223 MB already carried a 128 MB tmpfs swap export for a Pi that is not there. Both fixed:
the model file is now mapped (`gguf.c`, `model_q.c`) and the export is masked in the machine image.
The offload also gained the output head (row bands, 16-bit engine rows). Results in
`machine/README.md`.

## What is not done, said plainly

Linux has never reached user space on either FPGA board; the engines exist in simulation, QEMU and a
hash-verified card. The STM32 boards' vendor is unidentified until a silkscreen photo arrives. The P4
waits on eth1. The Teensy's new firmware and the 0.5B on its card wait for a flash and a card reader.
The first number the machine produces will be FPGA #1's boot report, and nothing above it is claimed
until that prints.
