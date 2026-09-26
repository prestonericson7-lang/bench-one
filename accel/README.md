# Accelerators for the Orange Pi — bring-up and measurement

What the Zynq and the Teensy add to the Orange Pi 4 Pro, and how to prove it on the bench.
Contracts: [SPEC.md](SPEC.md) (matrix engine + RAM tier) and [gpu/SPEC.md](gpu/SPEC.md) (your FPGA-GPU).

| Adds to the Pi | Where it runs | Pi talks to it through |
|---|---|---|
| **A GPU**: 3D rasterising, Z-buffer, sprites, HDMI 1280×720 out | Zynq PL (`accel/gpu/rtl`) + `fpgagpud` | `libfpgagpu`, `gpu_demo`, `gpu_selftest`, `gpu_view` — TCP 7777 |
| **A geometry engine**: transform, light, clip, triangle setup | Teensy 4.1 (`accel/gpu/teensy`) | USB serial from the Pi; 16-bit bus into the PL |
| **A matrix engine**: int4/int8 × int8, batch 8, weights held in the Zynq's DDR3 | Zynq PL (`accel/rtl/zaccel_gemv.v`) + `zaccel-server` | `libzaccel`, `zaccel.py`, `zaccel-bench` — TCP 8093 |
| **RAM**: part of the Zynq's 1 GB DDR3 as the Pi's swap, ahead of any disk swap | Zynq `nbd-server` | `zaccel-swap` — NBD, TCP 10809 |

One PL bitstream carries the GPU, the matrix engine, the platform registers and the second
Ethernet port together (`hardware/pz7020-starlite/vivado/build_system.tcl`).

## Zynq DDR3 map (1 GB)

| range | owner |
|---|---|
| 0x0000_0000 – 0x1DFF_FFFF | Linux (also backs the `zynqram` export) |
| 0x1E00_0000 – 0x1FFF_FFFF | GPU: framebuffers, sprite pool, frame-return buffer |
| 0x2000_0000 – 0x37FF_FFFF | matrix engine: tensors + DMA staging (384 MB) |
| 0x3800_0000 – 0x3FFF_FFFF | Linux |

## Bring-up on the bench (first time)

1. **Zynq**: the SD card carries the image (`hardware/pz7020-starlite/linux/out/pz7020-starlite-sd.img.xz`,
   sha256 in `sd-image.sha256`). Board setup and the hands-off boot capture:
   [BOOT-SD-runbook.md](../hardware/pz7020-starlite/BOOT-SD-runbook.md). The boot report must show
   `accel zaccel-server active fpgagpud active nbd-server active`, the three `uio` windows, and `pl_done yes`.
2. **Orange Pi**: copy `accel/accel-pi-bundle.tar.gz` over, then
   `mkdir -p ~/accel && tar -xzf accel-pi-bundle.tar.gz -C ~/accel && sudo bash ~/accel/install_pi.sh`.
3. **Teensy** (geometry engine) on the Pi's USB: `flash-teensy-gpu`. Wire it to JM1 per [gpu/WIRING.md](gpu/WIRING.md).
4. **Cables**: Pi Ethernet → Zynq PS RJ45; Zynq HDMI → a monitor.
5. **Measure** (on the Pi — only these numbers count):
   - `zaccel-bench -H 10.20.0.2` — Pi alone vs Zynq alone vs both at once, every answer checked.
   - `gpu_selftest` — the GPU bit-exact against the golden model, plus frames/s.
   - `swapon --show` — `/dev/nbd0` at priority 100 is the Zynq's RAM.

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
the Zynq's CPU there); the 32-bit / 1 GB DDR init; real throughput of each path and the combined
speedup; `nbd.ko` loading on the Pi's kernel (built from the vendor's own source and config, every
symbol checked against its System.map); eth1's RGMII timing.
