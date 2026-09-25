# PZ7020-StarLite — the Vivado flow (AMD tools, installed 2026-09-24)

Vivado 2026.1 + Vitis are installed at `D:\2026.1\` (`Vivado\bin\vivado.bat`). Everything below was
built on this PC with it; numbers are Vivado's own post-route reports.

## 1. The system design — `vivado/build_system.tcl`

```
D:\2026.1\Vivado\bin\vivado.bat -mode batch -source hardware\pz7020-starlite\vivado\build_system.tcl
```

| Block | What it is |
|---|---|
| `processing_system7` | Starts from the 169 PS7 parameters of the working PetaLinux build for this board (`vivado/ps7_validated.tcl`, applied in two passes), then: **DDR 32-bit = 1 GB** (2× MT41K256M16, DDR3L), UART0 115200, GEM0 MIO16-27 + MDIO 52/53, **GEM1 on EMIO**, USB0 reset MIO46, FCLK0 100 MHz, FCLK1 200 MHz, M_AXI_GP0. A hard check stops the build if any MIO assignment, bank voltage or the DDR width differs from the board. |
| `pl_regs` (module ref) | `ps7-axi/pl_regs.v` on M_AXI_GP0 at **0x4000_0000**: ID 0x5A702001, 64-bit time master, LEDs, keys, fan PWM + tach. |
| `gmii_to_rgmii` 4.1 | GEM1's GMII → RGMII to the PL-side RTL8211F (U16, MDIO address 2); converter at MDIO address 8; clocked from FCLK1 (IDELAYCTRL + its own MMCM). **TXC skew = 2 ns** because the RTL8211F datasheet (bundle, pin 24) gives TXDLY an internal pull-down and the schematic leaves that strap NC; RXDLY has an internal pull-up, so the PHY delays RX itself. |

Constraints: `vivado/system.xdc` (LEDs, keys, fan, RGMII, MDIO; `create_clock` on `rgmii_rxc` as PG160
requires — without it 18 RX capture registers were unclocked).

**Result (post-route):** 634 LUT, 1,039 FF, 2 DSP48, 0 BRAM, 20 IOB, 1 MMCM, 7 BUFG.
**WNS +0.198 ns, WHS +0.048 ns — timing met.** `check_timing`: 0 unclocked registers, 0 inputs without
delay (4 intentionally false-pathed). DRC: 4 DSP/clock-buffer warnings, 0 errors, 0 critical warnings.
Outputs: `vivado/build/system.bit` (938,343 B, compressed), `vivado/build/system.xsa` (371,725 B).

## 2. How it reaches the board

The boot chain that already ran on this board (U-Boot SPL → U-Boot → Linux, captured 2026-09-24,
`linux/captures/boot-20260924-161853`) is kept. What changed is what it carries:

| File on the card | Now |
|---|---|
| `boot.bin` (SPL) | compiled with **`ps7-vivado/ps7_init_gpl.c` from this XSA**: DDRC ctrl `0xF8006000 = 0x80` (32-bit), byte lanes 2-3 powered (`DDRIOB_DATA1 = 0x672`, was `0x800` off) |
| `u-boot.img` | DT memory `0x40000000` (1 GB) |
| `zImage` | + `CONFIG_XILINX_GMII2RGMII=y` |
| `zynq-pz7020-starlite.dtb` | 1 GB; `gem1` + converter@8 + PHY@2 (`eth1`); `pl_regs` as UIO; the 766 MHz operating point that fixed the cpufreq panic of the first boot |
| `pl.bit` | `vivado/build/system.bit`, loaded by U-Boot before Linux |

`linux/update_card.sh E` refreshes these six files on the card's BOOT partition and checks each
SHA-256 on the card after a cache flush. The full image (`linux/out/pz7020-starlite-sd.img.xz`) is
rebuilt from the same outputs.

**QEMU dry run** (`linux/qemu_test.sh`, 1 GB): kernel sees 1,048,576 KB, `eth1` registers on
GEM1, the converter driver binds and waits for PHY@2 (absent in QEMU, present on the board), login
prompt reached. QEMU cannot run ps7_init, the DDR PHY, the PHYs or the PL — the board run tests those.

**Fallback:** `linux/out/fallback-512MB/` is the 16-bit configuration that booted the board first
(third-party `ps7/ps7_init_gpl.c`, `0xF8006000 = 0x84`, 512 MB in every DT, `gem1` disabled). If the
32-bit set stops after the SPL banner, copy that folder's six files over the card's BOOT partition.

## 3. The SDR accelerator, signed off — `firmware/rtlsdr-pentest/fpga/vivado/build_bitstream.tcl`

404 LUT, 379 FF, 4 BRAM tiles, 4 DSP48, 7 IOB; **WNS +8.772 ns at 50 MHz** (≈89 MHz achievable),
WHS +0.052 ns, 0 failing endpoints. `build/sdr_accel.bit` 598,015 B. Same RTL the testbenches verify.

## 4. Rebuild order

1. `vivado/build_system.tcl` → `system.bit`, `system.xsa`
2. unzip `ps7_init_gpl.c/.h` from the XSA into `ps7-vivado/`
3. WSL: `build_uboot.sh` (checks: ps7_init symbols in the SPL, SPL DTB has UART + SD, prints the DDR width)
4. WSL: `build_kernel.sh`, then `mk_sd_image.sh`; or `update_card.sh E` for the boot files only
5. `build_fallback_512m.sh` keeps the 512 MB set current
