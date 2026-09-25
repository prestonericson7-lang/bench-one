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
| Board device tree (U-Boot + Linux) | [`linux/zynq-pz7020-starlite.dts`](linux/zynq-pz7020-starlite.dts) — every node from the manual/schematic/Vivado PS7 (1 GB, uart0, gem0 PHY 1 rgmii-id, gem1 → GMII-to-RGMII@8 → PHY 2, pl_regs UIO, 766 MHz OPP, qspi W25Q128, sdhci0, usb0 host + MIO46 reset) | — |
| SPL + U-Boot | `boot.bin`, `u-boot.img`, `boot.scr` | [`linux/build_uboot.sh`](linux/build_uboot.sh) |
| Kernel | `zImage`, `zynq-pz7020-starlite.dtb`, `modules.tar.gz` (v6.12 multi_v7 + FPGA manager, UIO, USB serial, Realtek PHY, QSPI NOR, NFS/CIFS) | [`linux/build_kernel.sh`](linux/build_kernel.sh) |
| Root filesystem | Debian bookworm armhf (systemd, ssh, python3 + serial/spidev, mtd/i2c/usb tools); hostname `zynq`, `root` / `zynq`, DHCP on eth0, serial getty on ttyPS0 | [`linux/build_rootfs.sh`](linux/build_rootfs.sh) |
| Boot script | [`linux/boot.cmd`](linux/boot.cmd) → `boot.scr` | mkimage |
| SD image | p1 FAT32 128 MiB (boot files + `pl.bit`), p2 ext4 (rootfs); assembled with mtools + `mke2fs -d`, no loop devices | [`linux/mk_sd_image.sh`](linux/mk_sd_image.sh) |
| PL at boot | `pl.bit` on the FAT partition = `vivado/build/system.bit` (Vivado: PS7 + register file at 0x40000000 + GMII-to-RGMII for `eth1`), loaded by U-Boot before Linux | [`linux/boot.cmd`](linux/boot.cmd) |
| PL bitstreams at runtime | `/lib/firmware/{fan_top,pz7020_ps7_top,sdr_accel_zynq_top}.bin` (header-stripped by [`linux/bit2bin.py`](linux/bit2bin.py)) → `echo pz7020_ps7_top.bin > /sys/class/fpga_manager/fpga0/firmware` | — |
| Talking to the PL from Linux | `pl_regs.py` (dump / `led` / `fan` / `time` over `/dev/mem`), `zynq_agent.py` publishes it to the Pi hub on :8091 | [`firmware/telemetry-hub/`](../../firmware/telemetry-hub/) |

## Writing and booting

1. Write `linux/out/pz7020-starlite-sd.img.xz` (decompress first) to a microSD with the unbuffered
   writer (`write-sd3.ps1` logic — the buffered path never reaches the card).
2. Boot jumper J1 → **SD** (MIO[5:4] = 11). Card in the slot on the underside.
3. Power via the **PWR+JTAG** Type-C; console on the **UART** Type-C (CH340, 115200 8N1).
4. Expect on the console: `U-Boot SPL 2025.07` → DDR init → `U-Boot 2025.07` → `Loading PL bitstream pl.bit` → kernel → `zynq login:`. Log in `root` / `zynq`.
5. LED1 (R19) blinks ~1 Hz the moment `pl.bit` is loaded by U-Boot — before Linux even starts.
   `ip addr` shows `eth0` up on the PS PHY. `pl_regs.py` prints `ID 0x5a702001` and a running
   `TIME`; `pl_regs.py led 3` lights LED2; `pl_regs.py fan 80` changes the fan.
6. QEMU dry run of the same kernel + rootfs (not the board): [`linux/qemu_test.sh`](linux/qemu_test.sh).

## What is verified and what is not

- ✅ The three build scripts run to completion on this PC (see the log lines recorded in
  [SYSTEM-INTEGRATION.md](SYSTEM-INTEGRATION.md) §4 once each finished).
- ✅ `ps7_init` symbols present in the SPL (checked by `nm` in `build_uboot.sh`).
- ✅ DTS compiles under both U-Boot's and Linux's dtc.
- ✅ **QEMU dry run boots to the login prompt** (`linux/qemu_test.sh`, 2026-09-24): our DTB, the 6.12
  kernel, the ext4 rootfs and systemd all work together; ttyPS0 is the console, root is `/dev/mmcblk0p2`.
- ⚠️ One real bug was found and fixed by that run: GNU tar replaces the merged-usr `/lib -> usr/lib`
  symlink with a directory when it extracts `lib/modules`, which hides `/lib/ld-linux-armhf.so.3` from
  every binary ("No working init found"). The scripts now use `tar --keep-directory-symlink` and put
  firmware under `usr/lib/firmware`; `linux/repair_rootfs_lib.sh` undoes the clobber on an existing tree.
- ✅ **Booted on the board, 2026-09-24** (`linux/captures/boot-20260924-161853`): SPL → DDR up →
  U-Boot → `pl.bit` loaded → kernel → panic in cpufreq (CPU at 766 MHz, not in the stock table). Fixed in
  the DTS with a single 766666 kHz operating point. That run used the 16-bit / 512 MB PS config.
- ⏳ **On the card now (2026-09-25), not yet booted:** the 1 GB set — SPL with the Vivado ps7_init
  (32-bit DDR), 1 GB DTs, kernel with `XILINX_GMII2RGMII`, Vivado `system.bit` ([VIVADO.md](VIVADO.md)).
  If it stops after the SPL banner, the 512 MB set in `linux/out/fallback-512MB/` is the known-good one.
- ⏳ First boot will also tell whether `eth0` needs `rgmii` instead of `rgmii-id`, and whether the
  2 ns TXC skew on `eth1` is right (RTL8211F strap defaults: TXDLY pull-down, RXDLY pull-up).
