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
the model file is now mapped (`gguf.c`, `model_q.c`), and the export was masked in the machine image.
The mask was undone on 2026-10-05: card #1 also serves the Orange Pi, and that export is the Pi's swap.
With it on, the board has 57 MB free of 223 instead of 149. Repeated that way, the two-board run
passed 8 of 11: at the engine-attach steps node 1 ran out of memory, and the kernel killed the engine
server and then `run_model`. The swap export and the machine's host role do not fit together in 223 MB
in emulation; whether they fit on the board, where the engine's memory is outside Linux, is unmeasured.
The offload also gained the output head (row bands, 16-bit engine rows). Results in
`machine/README.md`.

## The first real boot, 2026-10-05

The owner powered the Orange Pi, a Teensy and FPGA #1 (card #1). The FPGA's HDMI showed the PL's own
boot picture (8 colour bars and a square moving one pixel a frame, SPEC §8) and never turned dark blue,
which is what the GPU daemon does when it starts. Card #1 was then copied whole in the PC and read from
the copy (`machine/cards/read_cards.py`; the copy is `evidence/fpga1-card.img`).

What the card shows:

- **The board reached user space**, the first time it ever did: the SPL, the DDR3L setup, U-Boot, the
  bitstream, the kernel, the root filesystem and systemd all ran. Its own journal is on the card.
- **The journal stops at 5.55 s, but that is not when the board stopped.** 5.55 s is the moment journald
  first flushed its log to the card. Later entries were still in memory.
- **The board stopped between 8.0 s and about 13 s.** The card's ext4 journal holds three commits that
  were never written to their final place, at 5.634 s, 5.734 s and 8.014 s of uptime, and nothing after.
  The last one is systemd saving its random seed at 8.004 s. Systemd was still starting units then, so
  the next commit was due within five seconds. It never reached the card. (Method: the journal's wall
  clock minus its uptime gives the boot's start, 1777319332.506 s; each commit carries a wall-clock
  time.)
- **What was wrong at that moment.** At 1.91 s Linux logged `clk: Disabling unused clocks`. The device
  tree had `fclk-enable = <0x00>`, so nothing held FCLK0, the clock of the processor's bus into the PL.
  The Zynq clock driver turns off every PL clock nothing holds; Xilinx added `fclk-enable` in 2013 for
  exactly this case. A read over that bus with its clock stopped never finishes, and the CPU waits
  forever without logging anything. The services that read the PL start in the 8–13 s window.
- **That cause is inferred, not observed.** It fits every fact on the card, and the next boot decides it.

The fix, in three layers. The device tree now holds FCLK0 (`fclk-enable = <0x1>`), and the kernel
command line carries `clk_ignore_unused`, so either one alone keeps the clock running. A new boot guard,
`zynq-plcheck`, runs before anything reads the PL. It checks FCLK0's gate bit, the level shifters between
processor and PL, and the PL reset. Each step goes to `/boot/reports/plcheck.txt` with a sync, so the card
itself says how far the board got. If the bus would hang, it skips the PL read and holds back the
services that need it, so the board stays up and reachable. During bring-up, journald also writes to the
card every 2 s instead of every 5 minutes.

QEMU has no PL, so it cannot hang, but it keeps the clock registers Linux writes. Booted through the
production U-Boot with card #1's own `boot.scr` and device tree, Linux left FCLK0 unheld (enable count 0)
and set its gate bit (`FPGA0_THR_CNT = 1`). The guard named FCLK0 as the only problem, read nothing, held
the services back, and the board stayed up. With today's files the clock is held, the bit is clear, the
reads go through and the guard passes (`qemu_plcheck_test.sh`, 19 of 19). The full record of the card
reading is in `machine/cards/fpga1-first-boot-forensics.txt`.

The same day the Pi's card was read too. Its kernel log shows the NVMe drive holds **one partition, a
256 MB FAT filesystem with 159 MB on it**, which `nvme-auto` had mounted as "the NVMe". I had told the
owner it was a Windows laptop drive. That was a guess I never checked against the drive, and it was
wrong. Nothing on the drive is erased. `nvme-auto` now turns the drive's unpartitioned space into a new
ext4 partition and leaves the existing one exactly as it is. It was tested on that layout with this
PC's tools and with the Pi's own Ubuntu 26.04 tools.

## What is not done, said plainly

FPGA #1 reached user space once and then stopped, as above; no FPGA board has yet run a PL service, so
the engines exist in simulation, QEMU and a hash-verified card. The STM32 boards' vendor is unidentified
until a silkscreen photo arrives. The P4
waits on eth1. The Teensy's new firmware and the 0.5B on its card wait for a flash and a card reader.
The first number the machine produces will be FPGA #1's boot report, and nothing above it is claimed
until that prints.
