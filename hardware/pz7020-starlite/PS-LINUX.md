# Linux on the PZ7020-StarLite PS — built here, no Vivado, no PetaLinux

Everything in [linux/](linux/) runs inside the WSL2 Ubuntu-22.04 that was installed on this PC on
2026-09-24 (`wsl -d Ubuntu-22.04 -u root -- bash -c 'bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/<script>'`).
Sources live in `/root/zynq/` (U-Boot v2025.07, Linux v6.12); outputs land in `linux/out/`.

## How the PS gets initialised without an FSBL

The Zynq BootROM loads `boot.bin` from the SD card's first FAT partition. Ours is **U-Boot SPL**,
which compiles in [`ps7/ps7_init_gpl.c`](ps7/ps7_init_gpl.c) — the register-level PS
initialisation (PLLs, MIO mux, DDR PHY + controller, training) that Vivado generated for this exact
board in the hardware-validated `Hiroto-Nakano/PZ7020StarLite` build. U-Boot's
`board/xilinx/zynq/Makefile` picks it up by the device-tree name (`hw-platform-y := $(DEVICE_TREE)`),
so it sits in `board/xilinx/zynq/zynq-pz7020-starlite/`. SPL then loads `u-boot.img`, U-Boot's
default distro boot finds `boot.scr`, which loads an optional `pl.bit` into the fabric, then the
kernel and DTB.

| Piece | File | Script |
|---|---|---|
| Board device tree (U-Boot + Linux) | [`linux/zynq-pz7020-starlite.dts`](linux/zynq-pz7020-starlite.dts) — every node from the manual/schematic/validated PS7 (512 MB, uart0, gem0 PHY 1 rgmii-id, qspi W25Q128, sdhci0, usb0 host + MIO46 reset) | — |
| SPL + U-Boot | `boot.bin`, `u-boot.img`, `boot.scr` | [`linux/build_uboot.sh`](linux/build_uboot.sh) |
| Kernel | `zImage`, `zynq-pz7020-starlite.dtb`, `modules.tar.gz` (v6.12 multi_v7 + FPGA manager, UIO, USB serial, Realtek PHY, QSPI NOR, NFS/CIFS) | [`linux/build_kernel.sh`](linux/build_kernel.sh) |
| Root filesystem | Debian bookworm armhf (systemd, ssh, python3 + serial/spidev, mtd/i2c/usb tools); hostname `zynq`, `root` / `zynq`, DHCP on eth0, serial getty on ttyPS0 | [`linux/build_rootfs.sh`](linux/build_rootfs.sh) |
| Boot script | [`linux/boot.cmd`](linux/boot.cmd) → `boot.scr` | mkimage |
| SD image | p1 FAT32 128 MiB (boot files + `pl.bit`), p2 ext4 (rootfs); assembled with mtools + `mke2fs -d`, no loop devices | [`linux/mk_sd_image.sh`](linux/mk_sd_image.sh) |
| PL bitstreams at runtime | `/lib/firmware/fan_top.bin`, `/lib/firmware/sdr_accel_zynq_top.bin` (header-stripped by [`linux/bit2bin.py`](linux/bit2bin.py)) → `echo fan_top.bin > /sys/class/fpga_manager/fpga0/firmware` | — |

## Writing and booting

1. Write `linux/out/pz7020-starlite-sd.img.xz` (decompress first) to a microSD with the unbuffered
   writer (`write-sd3.ps1` logic — the buffered path never reaches the card).
2. Boot jumper J1 → **SD** (MIO[5:4] = 11). Card in the slot on the underside.
3. Power via the **PWR+JTAG** Type-C; console on the **UART** Type-C (CH340, 115200 8N1).
4. Expect on the console: `U-Boot SPL 2025.07` → DDR init → `U-Boot 2025.07` → `Loading PL bitstream pl.bit` → kernel → `zynq login:`. Log in `root` / `zynq`.
5. LED1 (R19) blinks ~1 Hz the moment `pl.bit` (the fan/heartbeat design) is loaded by U-Boot —
   before Linux even starts. `ip addr` shows `eth0` up on the PS PHY.

## What is verified and what is not

- ✅ The three build scripts run to completion on this PC (see the log lines recorded in
  [SYSTEM-INTEGRATION.md](SYSTEM-INTEGRATION.md) §4 once each finished).
- ✅ `ps7_init` symbols present in the SPL (checked by `nm` in `build_uboot.sh`).
- ✅ DTS compiles under both U-Boot's and Linux's dtc.
- ❌ **Not booted on the board yet** — the board was not attached. The first boot will tell whether
  the PHY needs `rgmii` instead of `rgmii-id` (change one line in the DTS if `eth0` gets no link).
- ⚠️ The PL-side Ethernet (`eth1`) needs AMD's GMII-to-RGMII IP in the fabric — Vivado only. Not
  in this image.
