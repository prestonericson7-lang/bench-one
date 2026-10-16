# The STM32H743 boards in the machine (phase 2)

Two "STM32H743IIT6 core boards with onboard SDRAM" (the listing: TKOWTB, 55 × 85 mm, dual USB-C, two LCD
ribbon sockets, TF slot, 2.0 mm SWD, "open source LVGL"). Recorded specs (`hardware/interconnect.md`):
Cortex-M7 at 480 MHz, 2 MB flash + 1 MB SRAM on the die, **32 MB SDRAM on the FMC bus**, 16 MB QSPI
flash, an EEPROM, a TF card slot, a CH340 USB-serial for flashing and console, a second USB-C on the
MCU's USB OTG. The layout in the photo is the FANKE-style design sold under many names; **the vendor
and the schematic are not identified yet**, and the pins below are what the firmware needs from them.

## Role

An **exact module**: the Teensy's `tl_core` (the 3B's proven core, `firmware/bench-one/shared/tl_core.c`)
on a second microcontroller family, streaming a model from its own card, with the 32 MB SDRAM as the
attention cache in place of the Teensy's eight PSRAM chips. Same line protocol as the Teensy (`I`,
`::gen`, a prompt, `step …` lines), so `verify_pc.py` and `suite.py` drive it unchanged. It joins the
machine over its OTG USB-C into an FPGA board's USB-A host port (cables 7b / 8b) as `/dev/ttyACMx`.

What it is for: two more exact checkers of the same model the engines run, on a different CPU from the
Teensy — the "third implementation" that turns two agreeing machines into a profile (docs/59 I2) — and
a measurement of what an FMC SDRAM (a real parallel bus) does for the cache against a bit-banged one.

## The port (`tl_plat.h`, five functions)

| function | Teensy today | STM32H743 |
|---|---|---|
| card read | SdFat FIFO 24.13 MB/s at 66 MHz; ADMA2 17.3 | SDMMC1, 4-bit, HAL `HAL_SD_ReadBlocks_DMA` (rate to measure; the H7's SDMMC does up to 50 MHz × 4 bits) |
| overlapped read | SdFat + ADMA2 with the arithmetic under it | IDMA (the SDMMC's own DMA) + the same hook |
| PSRAM banks | 8 chips, 8 MB each, bit-banged | **one bank of 32 MB**, memory-mapped on FMC (`0xC0000000`) — the row checksum and spare-slot logic apply with N = 1; a bank that is one contiguous RAM is the easy case |
| clock | `micros()` | DWT cycle counter / `HAL_GetTick` |
| log / serial | USB serial (Teensy) | CDC ACM on USB OTG FS (the second USB-C); console on USART → CH340 |

Cache arithmetic (measured per-position sizes, docs/55 and docs/59): the 3B needs 18 KB a position →
**about 1,700 positions** in 32 MB; a 0.5B (24 layers × 2 × 128) about 6 KB → **about 5,000**.

## Pins needed before any firmware (from the vendor schematic or the silkscreen)

- SDMMC1: `CK`, `CMD`, `D0..D3` (usually PC12, PD2, PC8..PC11 on H7 boards — to be confirmed, not assumed)
- FMC SDRAM: bank (1 or 2), `SDCKE`, `SDNE`, `SDCLK`, the 16 data lines, 13 address, 2 bank-address, `NBL0/1`, and the SDRAM part (W9825G6KH-6 is typical for 32 MB) for its timing table
- USB OTG FS: `PA11`/`PA12` and whether VBUS sensing is wired
- USART to the CH340: which USART and pins (USART1 PA9/PA10 is common)
- one LED, one user key, the HSE crystal frequency (8 or 25 MHz)

**Send a photo of the top and bottom silkscreen, or the listing / vendor link.** With it the pin table
is filled in a few minutes; without it nothing is flashed.

## Toolchain

`arm-none-eabi-gcc` from the Teensy toolchain (`%LOCALAPPDATA%/Arduino15/packages/teensy/tools/teensy-compile`)
plus STM32CubeH7's CMSIS device headers and HAL for SDMMC, FMC and USB device — fetched into
`machine/stm32/third_party/` with its licence row in THIRD-PARTY-NOTICES when phase 2 starts. Flashing:
the ROM bootloader over the CH340 (`stm32flash`, BOOT0 high) or SWD if a probe is on hand.

## Arithmetic, labelled

A 480 MHz M7 does the Teensy's arithmetic at 0.8×: the 3B's 28.8 s a token becomes about 36 s, so with
a card at (say) 20 MB/s a 3B token is roughly 96 + 36 ≈ 130 s; a 0.5B Q8_0 file (676 MB) about 34 + 6 ≈
40 s a token, or eight positions per pass in about 60 s. All arithmetic until the card rate is measured.

## Bring-up, one instrumented build per question (the flash-loop rule stands)

1. blink + console over the CH340: the board answers.
2. SDMMC: read 64 MB sequentially, byte-checked against the PC; the rate is the number.
3. SDRAM: fill all 32 MB with tagged data, read back, count wrong bytes — the same proof the Teensy runs.
4. `tl_core` on the France prompt: step lines equal to `tl_ref`. Then the time.
5. USB CDC into an FPGA's USB-A: `machine-bench teensy` finds it as `/dev/ttyACM*` and prints its `I` line.
