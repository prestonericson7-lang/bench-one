# Accelerators for the Orange Pi — bring-up and measurement

What the Zynq and the Teensy add to the Orange Pi 4 Pro, and how to prove it on the bench.
Contracts: [SPEC.md](SPEC.md) (matrix engine + RAM tier) and [gpu/SPEC.md](gpu/SPEC.md) (your FPGA-GPU).

| Adds to the Pi | Where it runs | Pi talks to it through |
|---|---|---|
| **A GPU**: 3D rasterising, Z-buffer, sprites, HDMI 1280×720 out | Zynq PL (`accel/gpu/rtl`) + `fpgagpud` | `libfpgagpu`, `gpu_demo`, `gpu_selftest`, `gpu_view` — TCP 7777 |
| **A geometry engine**: transform, light, clip, triangle setup | Teensy 4.1 (`accel/gpu/teensy`) | USB serial from the Pi; 16-bit bus into the PL |
| **A matrix engine**: int4/int8 × int8, batch 8, weights held in the Zynq's DDR3 | Zynq PL (`accel/rtl/zaccel_gemv.v`) + `zaccel-server` | `libzaccel`, `zaccel.py`, `zaccel-bench` — TCP 8093 |
| **RAM**: 128 MB of the Zynq's 512 MB DDR3 as the Pi's swap, ahead of any disk swap | Zynq `nbd-server` | `zaccel-swap` — NBD, TCP 10809 |

One PL bitstream carries the GPU, the matrix engine, the platform registers and the second
Ethernet port together (`hardware/pz7020-starlite/vivado/build_system.tcl`).

## Zynq DDR3 map (512 MB)

The board has ONE x16 DDR3L chip, MT41K256M16TW-107IT:P = 512 MB, on a 16-bit bus: the schematic wires only
DQ0-15 and A0-A14, the vendor's only DRAM datasheet is that part, and the chip in the vendor photo is marked
D9SHG (= that part). The listing's "1GB" is wrong. The 32-bit / 1 GB PS config was silent on the board.

| range | owner |
|---|---|
| 0x0000_0000 – 0x0FFF_FFFF | Linux, 256 MB (also backs the 128 MB `zynqram` export; Linux + services use ~42 MB) |
| 0x1000_0000 – 0x1DFF_FFFF | matrix engine: tensors + DMA staging (224 MB) |
| 0x1E00_0000 – 0x1FFF_FFFF | GPU: framebuffers, sprite pool, frame-return buffer (fixed in the PL) |

## Bring-up on the bench (first time)

1. **Zynq**: the SD card carries the image (`hardware/pz7020-starlite/linux/out/pz7020-starlite-sd.img.xz`,
   sha256 in `sd-image.sha256`). Board setup and the hands-off boot capture:
   [BOOT-SD-runbook.md](../hardware/pz7020-starlite/BOOT-SD-runbook.md). The boot report must show
   `accel zaccel-server active fpgagpud active nbd-server active`, the three `uio` windows, and `pl_done yes`.
2. **Orange Pi**: its card already carries the bundle (the official 1.1.0 image plus the bundle,
   [build_pi_card_image.sh](build_pi_card_image.sh), written with `write_sd.py`). At its first boot the Pi
   installs everything by itself and writes the log to `~/accel-install.log`. That includes the two missing
   packages (shipped in the bundle, no internet needed) and the NVMe drive: a blank drive is partitioned,
   formatted ext4 and mounted at **/mnt/nvme**, at every boot from then on (`nvme-auto`; a drive that
   already holds data is never formatted, and an EFI boot partition left by another OS does not count as
   storage -- such a drive is left alone and the log prints the one command that hands it to the Pi).
   The Pi's actual drive (Samsung MZVLB256HAHQ-000H1, 238.5 GB), from the Pi's own logs read off its
   card 2026-10-05: **one partition** (`nvme0n1: p1`) holding a 256 MB FAT filesystem with 159 MB on it,
   mounted as "the NVMe" by both nvme-auto versions so far. nvme-auto now makes the drive's unpartitioned
   space -- if it is bigger than the largest existing filesystem -- a new ext4 "nvme" partition and
   mounts that, changing nothing already on the drive, and logs the layout it found to
   `/var/lib/accel/nvme-auto.log` (`pi/test_nvme_pi_drive.sh`: that layout as GPT basic-data, MBR FAT32
   and EFI-typed, every file of the FAT partition checked byte for byte afterwards; run with this PC's
   tools and, with `PI_IMG=`, inside the Pi's own Ubuntu 26.04 userland with its util-linux 2.41.3).
   Nothing is erased. That second run found that the Pi's `sfdisk` reports an MBR type as `ef`, not
   `0xef`, so an MBR EFI partition would have counted as storage; fixed, and the test fails on the old
   file and passes on the new one.
   To give a whole drive to the Pi (it **erases** it), the log prints the command:
   `sudo umount /dev/nvme0n1p* ; sudo wipefs -a /dev/nvme0n1 && sudo dd if=/dev/zero of=/dev/nvme0n1 bs=1M count=1 conv=fsync && sudo systemctl restart nvme-auto`
   (`pi/test_nvme_claim.sh`; the unmount first, because a drive held by a mount makes `wipefs` refuse).
   The packages install even while the vendor's own first-run holds the package database (it did on the
   first real boot, 2026-09-26), and the install leaves the Pi's `/boot` files untouched. The card also carries a test model,
   `/opt/accel/models/qwen2.5-coder-3b.gguf` (the Ollama `qwen2.5-coder:3b` blob), so `bench-day` with no
   argument runs the real-model comparison too. Card image: `MODEL=<blob> MODEL_NAME=qwen2.5-coder-3b.gguf
   bash accel/build_pi_card_image.sh`, sha256 `a9b64e79d87bdd1ea5ebd88d499e48586055ab5859a5f19b90c8cd5d7f27e3ec` (2026-09-26 17:23: the debconf-race and EFI-partition fixes; replaces `3c3afdda...`, whose first boot on the Pi left nbd-client half-configured).
   On a card without it: copy `accel/accel-pi-bundle.tar.gz`
   over, then
   `mkdir -p ~/accel && tar -xzf accel-pi-bundle.tar.gz -C ~/accel && sudo bash ~/accel/install_pi.sh`.
   Run it from the Pi's own desktop or over Wi-Fi, not over SSH on the Pi's Ethernet. The installer gives
   the Pi's wired port the fixed address **10.77.0.1** for the Zynq cable (NetworkManager profile
   `fpgagpu`, kept across reboots), so a session on that port drops partway through, and from then on
   the Pi's internet is Wi-Fi. Undo: `sudo nmcli connection delete fpgagpu`.
3. **Teensy** (geometry engine) on the Pi's USB: `flash-teensy-gpu`. Then unplug the Teensy's USB and the
   Zynq's J8, and wire it to JM1 with both boards unpowered, per [gpu/WIRING.md](gpu/WIRING.md)
   (grounds first; it also gives the power-up and power-down order).
4. **Cables**: Pi Ethernet → the Zynq's upper RJ45 (**ETH-PS**); Zynq HDMI → a monitor. Every pin and port:
   seven one-screen pages in [hardware/pz7020-starlite/wiring-png/](../hardware/pz7020-starlite/wiring-png/)
   (in this order, cables, JM1, Teensy, wire by wire W1–W22), drawn from `system-wiring.svg`.
5. **Measure** (on the Pi — only these numbers count). `bench-day [model.gguf]` runs all of these and saves
   `~/accel-bench-<date>.txt`:
   - `zaccel-bench` — Pi alone vs Zynq alone vs both at once, every answer checked. Leave out `-H`:
     with no host it tries 10.20.0.2 (car LAN), then 10.77.0.2, which is where the Zynq answers on the
     direct cable. `-H 10.77.0.2` names it directly.
   - `gpu_selftest` — the GPU bit-exact against the golden model, plus frames/s.
   - `swapon --show` — `/dev/nbd0` at priority 100 is the Zynq's RAM.
   - A real model, Pi alone vs Pi + Zynq (copy a GGUF over, e.g. the Ollama `qwen2.5-coder:3b` blob):
     `run_model m.gguf "def fibonacci(n):" 64 --fast` then the same with `--zaccel auto` — compare
     the `decode` and `prefill` tok/s lines, and `ppl m.gguf --fast --limit 256 [--zaccel auto]` for quality.

## The model runtime uses the matrix engine

`run_model` and `ppl` (the repo's own runtime, `firmware/bench-one/shared/model_q.c`) take
`--zaccel HOST[:PORT] [--share S]`. A share of every dense matrix's rows is requantized to int8 (one
scale per row) and held in the Zynq's DDR3; each matrix-vector product sends the activation there and
computes the rest on the Pi's cores at the same time. Prompt processing sends positions 8 at a time, so
one weight read on the Zynq serves 8 positions. Without `--share` the split is **measured** at start:
both sides are timed on a real matrix and each matrix gets the share that finishes both halves
together, or none when the network round trip alone costs more than the Pi needs for the whole matrix.
The Zynq's free memory caps it. If the Zynq stops answering, its rows are recomputed on the Pi and the
offload switches off.

Activations carry outlier channels, so both the Zynq's weight rows and every activation are rotated
by a block Hadamard matrix first (orthonormal, so exact), and each engine column chunk gets its own
activation scale. Without the rotation the model broke (perplexity 13.9 → 174); with it, measured on
Qwen2.5-Coder 3B with `ppl.c` (64 tokens): **19.14 CPU-only, 19.36 half the rows on the Zynq path,
19.03 all of them**, top-1 unchanged, and the generated text identical. `accel/llm/run_tests.sh`
repeats all of it.

int4 weights (`ZACCEL_WBITS=4`, one scale per row) were measured and **rejected**: perplexity 19.14 →
29.91 with half the rows, 95.95 with all, even rotated. int4 needs a scale per small group of weights,
which the engine does not have; int8 is the default and the only setting to use.

```
run_model model.gguf "def fibonacci(n):" 64 --fast --zaccel auto   # n_tokens must be 3rd
ppl model.gguf --fast --limit 256 --zaccel auto                   # the quality cost
```

## Proven here, and what only the boards can answer

Proven on this PC: the matrix engine RTL bit-exact under four flow-control patterns (6/6 planted bugs
caught); the engine behind the **real Xilinx AXI DMA IP** with its board reset wiring, driven by the
server's exact register sequence, is exact on every job, including chunked weight transfers and
recovery after an aborted job (`cosim/run_cosim.ps1`, xsim); the merged bitstream closes timing on every clock; the GPU's RTL, daemon, geometry and Pi tools
pass their own suites from here; `zaccel-server` passes on x86 and on the ARM binary, including its PL
path through a software model of the DMA + engine; the Pi client and benchmark pass on x86 and aarch64;
the RAM export and `zaccel-swap` pass under a real kernel (swap in use, pages back intact); and the real
SD image boots in QEMU with every service answering the Pi-side tools (`qemu_accel_test.sh`).

Only the boards can answer: the fabric running on silicon (QEMU has no PL, so the engine answers from
the Zynq's CPU there); the DDR init runs the 16-bit config that booted this board; real throughput of each path and the combined
speedup; `nbd.ko` loading on the Pi's kernel (built from the vendor's own source and config, every
symbol checked against its System.map); eth1's RGMII timing.
