# PZ7020-StarLite — the Vivado flow (AMD tools, installed 2026-09-24)

Vivado 2026.1 + Vitis are installed at `D:\2026.1\` (`Vivado\bin\vivado.bat`). Everything below was
built on this PC with it; numbers are Vivado's own post-route reports.

## 0. One bitstream for the Orange Pi's accelerators (2026-09-25)

The PL can hold one configuration at a time, so the system design now carries everything together:
the platform below, **the FPGA-GPU** (`accel/gpu/rtl`, top `gpu_pl`, its own MMCM from U18: core
148.75 MHz, HDMI 720p60, Teensy 16-bit bus on JM1) and **the matrix engine** (`accel/rtl/zaccel_gemv.v`
behind an AXI DMA). Top: `system_top` joins `gpu_pl` and the block design.

| Address (GP0) | Block | Memory side |
|---|---|---|
| 0x4000_0000, 4 KB | `pl_regs` | — |
| 0x4040_0000, 64 KB | AXI DMA (simple mode, 64-bit streams, 26-bit length) + `zaccel_gemv` (FCLK0 100 MHz) | S_AXI_HP3 |
| 0x43C0_0000, 4 KB | GPU registers + PS command FIFO | S_AXI_HP0 (scanout), HP1 (strip writes), HP2 (sprites), GPU clock |

The engine's `aresetn` = system reset AND the DMA's MM2S/S2MM stream resets, so a DMA soft reset also
clears an aborted job. LEDs belong to the GPU (heartbeat, frame toggle).

**Result (post-route, `vivado/build/system_summary.txt`):** 23,571 LUT (44 %), 19,167 FF, 93.5 of 140
BRAM tiles, 2 DSP48, 50 IOB, 2 MMCM. By block: GPU 7,712 LUT / 74 BRAM tiles; matrix engine 11,773 LUT /
16 RAMB36 (its 128 8×8 multipliers went to LUTs, not DSP48s); AXI DMA 1,643; GP0 interconnect 1,652.
**All constraints met: WNS +0.198 ns, WHS +0.042 ns**; GPU core clock (148.75 MHz) WNS +0.621 ns,
FCLK0 (100 MHz) WNS +0.957 ns. 0 critical warnings. `ps7_init_gpl.c/.h` from the new XSA are
**identical** to `ps7-vivado/`, so the SPL does not change. `system.bit` 2,498,199 B, `system.xsa`
1,148,976 B. Pin check: `tools/check_xdc.py` — all 50 PACKAGE_PIN lines match the vendor tables, no
ball used twice.

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

| File on the card | Now (2026-09-26) |
|---|---|
| `boot.bin` (SPL) | **`ps7/ps7_init_gpl.c`, 16-bit DDR (`0xF8006000 = 0x84`)** -- byte-identical code to the SPL that booted the board on 2026-09-24 (only the 6-byte build stamp differs). `build_uboot.sh` refuses `ps7-vivado` |
| `u-boot.img` | DT memory `0x20000000` (512 MB) |
| `boot.scr` | stages `pl.bit` at `0x08000000`, zImage at `0x03000000`, DT at `0x02E00000` -- clear of the reserved windows (U-Boot refuses to load into reserved memory) and of the unpacked kernel (it ends at `0x01FF8B10`; the default DT address `0x1F00000` was inside it) |
| `zImage` | + `CONFIG_XILINX_GMII2RGMII=y` |
| `zynq-pz7020-starlite.dtb` | 512 MB; engine window `0x10000000` (224 MB), GPU `0x1E000000` (32 MB); `gem1` off (the 16-bit PS config does not clock GEM1/FCLK1); `pl_regs` + engine as UIO; the 766 MHz operating point |
| `pl.bit` | `vivado/build/system.bit`, loaded by U-Boot before Linux |

**Why not the Vivado PS config:** this design's PS7 says 32-bit / 1 GB (the listing's claim). The board has one
x16 MT41K256M16 (512 MB): the SPL built from `ps7-vivado/` gave zero console bytes on 2026-09-26. The PL
bitstream does not depend on the DDR width, so `system.bit` is unchanged; `ps7-vivado/` is not used.

**Tested without the board:** `linux/qemu_uboot_test.sh` runs the production U-Boot and `boot.scr` from
the image at 512 MB into Linux; `linux/qemu_accel_test.sh` boots the image at 512 MB with every service
answering. QEMU cannot run the BootROM, ps7_init, the DDR PHY or the PL -- the board run tests those.

## 3. The SDR accelerator, signed off — `firmware/rtlsdr-pentest/fpga/vivado/build_bitstream.tcl`

404 LUT, 379 FF, 4 BRAM tiles, 4 DSP48, 7 IOB; **WNS +8.772 ns at 50 MHz** (≈89 MHz achievable),
WHS +0.052 ns, 0 failing endpoints. `build/sdr_accel.bit` 598,015 B. Same RTL the testbenches verify.

## 4. Rebuild order

1. `vivado/build_system.tcl` → `system.bit`, `system.xsa`
2. (the XSA's `ps7_init` is NOT used: 32-bit DDR does not fit the board -- see above)
3. WSL: `build_uboot.sh` (uses `ps7/` 16-bit; checks: ps7_init symbols in the SPL, SPL DTB has UART + SD,
   refuses any DDR width but 16-bit)
4. WSL: `build_kernel.sh`, then `mk_sd_image.sh`; or `update_card.sh E` for the boot files only
5. WSL: `qemu_uboot_test.sh` + `qemu_accel_test.sh` (in `deploy/run_all_tests.sh`)
