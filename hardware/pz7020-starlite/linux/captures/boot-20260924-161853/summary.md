# PZ7020-StarLite boot capture, 2026-09-24 16:19:59

Result: **kernel panic** (exit 3). Link: COM36 115200 8N1, DTR/RTS not asserted. Times are host seconds from the first console byte.

## How far it got

| t (s) | stage | console line |
|---:|---|---|
| 0.00 | SPL banner: BootROM loaded BOOT.BIN from the SD card; ps7_init (clocks, DDR, MIO) is next | `U-Boot SPL 2025.07-dirty (Sep 24 2026 - 15:53:16 -0700)` |
| 0.47 | U-Boot proper: SPL brought DDR up and loaded u-boot.img | `U-Boot 2025.07-dirty (Sep 24 2026 - 15:53:16 -0700)` |
| 0.47 | U-Boot sized the DDR | `DRAM:  ECC disabled 512 MiB` |
| 2.73 | U-Boot found boot.scr on the SD card | `Found U-Boot script /boot.scr` |
| 2.94 | boot.scr is loading pl.bit into the fabric | `Loading PL bitstream pl.bit (3dbb72 bytes)` |
| 3.72 | U-Boot handed over to Linux | `Starting kernel ...` |
| 4.53 | Linux kernel running | `[    0.000000] Booting Linux on physical CPU 0x0` |
| 5.09 | Linux sees its memory | `[    0.183662] Memory: 421944K/524288K available (16384K kernel code, 2610K rwdata, 7108K rodata, 2048K init, 432K bss, 35020K reserved, 65536K cma-reserved, 0K highmem)` |
| 6.79 | KERNEL PANIC | `[    1.948397] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b` |

Stages not seen: rootfs, systemd, login, report

## What the watcher typed

- nothing

## Board report

None captured.

## U-Boot console, verbatim (SPL banner to kernel handoff)

```

U-Boot SPL 2025.07-dirty (Sep 24 2026 - 15:53:16 -0700)
Silicon version:	3
Trying to boot from MMC1
spl_load_image_fat_os: error reading image system.dtb, err - -2


U-Boot 2025.07-dirty (Sep 24 2026 - 15:53:16 -0700)

CPU:   Zynq 7z020
Silicon: v3.1
Model: Puzhi PZ7020-StarLite
DRAM:  ECC disabled 512 MiB
Core:  22 devices, 16 uclasses, devicetree: board
Flash: 0 Bytes
NAND:  0 MiB
MMC:   mmc@e0100000: 0
Loading Environment from FAT... *** Error - No Valid Environment Area found
*** Warning - bad env area, using default environment

In:    serial@e0000000
Out:   serial@e0000000
Err:   serial@e0000000
Net:
ZYNQ GEM: e000b000, mdio bus e000b000, phyaddr 1, interface rgmii-id

Warning: ethernet@e000b000 (eth0) using random MAC address - 5a:2e:cf:79:e1:75
eth0: ethernet@e000b000
Hit any key to stop autoboot:  2  1  0
switch to partitions #0, OK
mmc0 is current device
Scanning mmc 0:1...
Found U-Boot script /boot.scr
938 bytes read in 11 ms (83 KiB/s)
## Executing script at 03000000
4045682 bytes read in 238 ms (16.2 MiB/s)
Loading PL bitstream pl.bit (3dbb72 bytes)
  design filename = "build\pz7020_ps7_top.frames;Generator=xc7frames2bit"
  part number = "xc7z020clg400-2"
  date = "2026/09/24"
  time = "09:19:04"
  bytes in bitstream = 4045564
zynq_align_dma_buffer: Align buffer at 10000076 to 10000040(swap 1)
INFO:post config was not run, please run manually if needed
11846144 bytes read in 665 ms (17 MiB/s)
11371 bytes read in 12 ms (924.8 KiB/s)
Kernel image @ 0x2000000 [ 0x000000 - 0xb4c200 ]
## Flattened Device Tree blob at 01f00000
   Booting using the fdt blob at 0x1f00000
Working FDT set to 1f00000
   Loading Device Tree to 1dabc000, end 1dac1c6a ... OK
Working FDT set to 1dabc000

Starting kernel ...

```

## Console lines that say error / fail / timeout / warn, outside the reports (first 40, not interpreted)

```
    0.13  spl_load_image_fat_os: error reading image system.dtb, err - -2
    0.58  Loading Environment from FAT... *** Error - No Valid Environment Area found
    0.58  *** Warning - bad env area, using default environment
    0.58  Warning: ethernet@e000b000 (eth0) using random MAC address - 5a:2e:cf:79:e1:75
    4.64  [    0.000000] efi: UEFI not found.
    6.23  [    1.437813] cpu cpu0: _opp_config_clk_single: failed to set clock rate: -16
    6.34  [    1.444820] cpufreq: __target_index: Failed to change cpu frequency: -16
    6.34  [    1.461625] Internal error: Oops - BUG: 0 [#1] SMP ARM
    6.79  [    1.948397] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
    6.79  [    1.956129] ---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b ]---
```

## Last 30 console lines

```
    6.56  [    1.774292] 5f80: c12ca190 00000000 00000000 00000000 00000000 00000000 00000000 c12ca1ac
    6.68  [    1.782460] 5fa0: 00000000 c12ca190 00000000 c03001ac 00000000 00000000 00000000 00000000
    6.68  [    1.790627] 5fc0: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
    6.68  [    1.798795] 5fe0: 00000000 00000000 00000000 00000000 00000013 00000000 00000000 00000000
    6.68  [    1.806959] Call trace:
    6.68  [    1.806971]  cpufreq_online from cpufreq_add_dev+0xa8/0xb8
    6.68  [    1.814988]  cpufreq_add_dev from subsys_interface_register+0xfc/0x118
    6.68  [    1.821526]  subsys_interface_register from cpufreq_register_driver+0x164/0x2fc
    6.68  [    1.828842]  cpufreq_register_driver from dt_cpufreq_probe+0x2cc/0x3f0
    6.68  [    1.835379]  dt_cpufreq_probe from platform_probe+0x5c/0xbc
    6.68  [    1.840968]  platform_probe from really_probe+0xc8/0x2c8
    6.68  [    1.846289]  really_probe from __driver_probe_device+0x88/0x19c
    6.68  [    1.852217]  __driver_probe_device from driver_probe_device+0x30/0x104
    6.68  [    1.858753]  driver_probe_device from __driver_attach+0x90/0x174
    6.68  [    1.864768]  __driver_attach from bus_for_each_dev+0x70/0xc4
    6.68  [    1.870428]  bus_for_each_dev from bus_add_driver+0xcc/0x1ec
    6.68  [    1.876087]  bus_add_driver from driver_register+0x7c/0x114
    6.68  [    1.881668]  driver_register from do_one_initcall+0x48/0x1f4
    6.68  [    1.887338]  do_one_initcall from kernel_init_freeable+0x1bc/0x220
    6.68  [    1.893527]  kernel_init_freeable from kernel_init+0x1c/0x12c
    6.79  [    1.899290]  kernel_init from ret_from_fork+0x14/0x28
    6.79  [    1.904348] Exception stack(0xe0815fb0 to 0xe0815ff8)
    6.79  [    1.909394] 5fa0:                                     00000000 00000000 00000000 00000000
    6.79  [    1.917563] 5fc0: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
    6.79  [    1.925731] 5fe0: 00000000 00000000 00000000 00000000 00000013 00000000
    6.79  [    1.932340] Code: e3a02000 ebfff837 e3500000 0a000009 (e7f001f2)
    6.79  [    1.938429] ---[ end trace 0000000000000000 ]---
    6.79  [    1.943036] note: swapper/0[1] exited with irqs disabled
    6.79  [    1.948397] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
    6.79  [    1.956129] ---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b ]---
```

Every byte the board sent is in console.log next to this file.
