# PZ7020-StarLite — Zynq PS configuration, from the schematic

What the `ZYNQ7 Processing System` block in Vivado has to be told so that the FSBL brings up
DDR, QSPI, SD, UART, USB and Ethernet on **this** board. Every value is tagged with where it
comes from. The companion Tcl is [`constraints/ps7_starlite.tcl`](constraints/ps7_starlite.tcl).

Sources: `Puzhi PZ-StarLite Schematic.pdf` V1.0 (Feb/Mar 2025) sheets 2, 4, 5, 7, 8, 15, 16;
User Manual V1.0 Parts 3.2–3.12; Micron `MT41K256M16TW-107` datasheet (all in the vendor bundle).

---

## 1. DDR width — SETTLED 2026-09-24 (16-bit, 512 MB, DDR3L)

> A working PetaLinux 2024.1 build for this exact board (`Hiroto-Nakano/PZ7020StarLite`, MIT; files
> in [ps7/](ps7/)) uses **`16 Bit`, `DDR 3 (Low Voltage)`, HIGHADDR 0x1FFFFFFF = 512 MB**, and records
> that the 1.5 V `DDR 3` setting fails with `DDR_INIT_FAIL`. That matches schematic V1.0 below and
> makes the manual's "1 GB" wrong for this board. The physical check is no longer needed; the
> analysis is kept for the record.

| | Schematic V1.0 says | Manual table says |
|---|---|---|
| DRAM | **one** Micron `MT41K256M16TW-107IT` (U9, sheet 8): 4 Gb, ×16 | "DDR3: PZ7010 512 MB / PZ7020 1 GB" |
| Bus | **16-bit** — only `PS_DDR3_DQ0..15`, `DQS0/1`, `DM0/1` are routed (sheet 7); the Zynq's `DQ16..31` balls are unconnected | — |
| Capacity | **512 MB** | 1 GB |

These cannot both be true of the same board. The Zynq-7000 DDRC has address lines A0–A14 only
(sheet 7 stops at `PS_DDR_A14_502`), so a single ×16 chip is capped at 4 Gb = 512 MB; 1 GB needs
**two** 4 Gb ×16 chips on a **32-bit** bus. The vendor may populate the 7020 differently from this
schematic revision.

**Physical check (10 s):** count the DRAM packages next to the Zynq (96-ball FBGA, ~8 × 13 mm,
Micron marking `D9…` / `MT41K256M16`). One chip → 16-bit / 512 MB. Two → 32-bit / 1 GB.
Then set `PCW_UIPARAM_DDR_BUS_WIDTH` accordingly (§3). A wrong width either fails DDR training
in the FSBL or silently halves usable memory. ⚠️ The "1 GB" line in this repo's README and in the
owner's board repo is unverified against the silicon; this check settles it.

**DDR rail voltage:** the power sheet (2) sets one MP2143 to **1.35 V** (`VDD_1V35`, test point
TP3) and the DDR sheets use `VDD_1V5`/`VTT_DDR3`/`VREF_DDR3` net names. The `MT41K…TW` is a DDR3L
part (1.35 V nominal, 1.5 V tolerant). Measure TP3: 1.35 V → memory type **DDR 3 (Low Voltage)**;
1.5 V → **DDR 3**. Either trains; the flag sets the DDR I/O drive calibration.

## 2. Boot straps and bank voltages (sheet 5) ✅ DOC

| Strap | Value | Meaning |
|---|---|---|
| MIO[7] | 0 | **MIO bank 0 (MIO0–15) = 3.3 V** |
| MIO[8] | 1 | **MIO bank 1 (MIO16–53) = 1.8 V** |
| MIO[6] | 0 | PLL used |
| MIO[3] | 0 | JTAG / NAND / Quad-SPI / SD boot family |
| MIO[2] | 0 | cascaded JTAG |
| MIO[5:4] | jumper J1 | `00` JTAG · `10` QSPI · `11` SD card |

The PS reference clock is **33.333333 MHz** (Y1, `PS_CLK_500` ball E7) [M 3.2, sheet 4].
`PS_POR_B` (C7) is the nGRST key [M 3.3]. `PS_SRST_B` (B10) is on the JTAG sheet.

## 3. MIO map ✅ DOC (User Manual Parts 3.5, 3.6, 3.10–3.12; sheet 5)

| Peripheral | MIO | Notes |
|---|---|---|
| QSPI (W25Q128JV, 128 Mb) | **1–6** (CS=1, DQ0–3=2..5, CLK=6); MIO8 = FBCLK/strap | single SS, 3.3 V |
| UART0 (CH340E console) | **10 = RX, 11 = TX** | 3.3 V, 115200 8N1 |
| GEM0 (PS Ethernet, RTL8211F U15, **PHY addr 1**) | **16–27** RGMII, **MDIO 52/53** | 1.8 V bank; PHY LDO strap `CFG_LDO` on the PHY sets its RGMII I/O to match |
| USB0 (USB3320C ULPI, host) | **28–39**, reset **MIO46** | 1.8 V |
| SD0 (microSD, level-shifted) | **40–45** (CLK 40, CMD 41, D0–3 42..45) | no CD/WP line documented |
| GEM1 (PL-side PHY U16, **PHY addr 2**) | **EMIO** → GMII-to-RGMII in the PL (see §5) | BANK34 pins, 3.3 V |

## 4. Recommended PS7 settings (what the Tcl sets)

| Setting | Value | Why |
|---|---|---|
| APU clock | 766.67 MHz | 7020-2 speed grade [M 2.2]; use 666.67 if the chip marking says -1 |
| DDR part | `MT41K256M16 RE-125` (closest catalogue entry to TW-107; same 4 Gb ×16 organisation) | DDR clock 533.33 MHz (DDR3-1066, Zynq-7000 maximum); a -107/-125 part meets 1066 timing with margin |
| DDR bus width | **16 Bit** per the schematic — change to **32 Bit** only if the physical check finds two chips | §1 |
| DDR ECC | disabled | ×16 bus, no ECC lanes |
| Bank 0 / bank 1 | LVCMOS 3.3 V / LVCMOS 1.8 V | sheet 5 straps |
| FCLK_CLK0 | 100 MHz to the PL | general PL clock alongside the 50 MHz oscillator |
| M_AXI_GP0, S_AXI_HP0 | enabled | control + bulk paths for the accelerators |

**Training:** leave DQS-to-clock and board-delay training enabled (defaults). The vendor's own
values would be in the course-demo projects, but the `4_HLS.rar` in the bundle is truncated
(Google Takeout split it across four zips and only part 1 was supplied) — obtain parts 2–4 if
training ever proves marginal.

## 5. PL-side Ethernet — Linux gets a second port without writing a MAC

The PL PHY is an ordinary RGMII PHY on BANK34 (pins in `pz7020_starlite_board.xdc`). Enable
**GEM1 on EMIO** in PS7 and drop AMD's **GMII to RGMII** IP (shipped with Vivado, no separate
licence) between GEM1's GMII and the RGMII pins; set the IP's PHY address to **2** and, in the
device tree, `phy-mode` to `rgmii-id` first (the PHY's TX/RX delay straps R144/R146 are marked
NC, so the delay mode is not fixed by the schematic — try `rgmii` if `rgmii-id` gives no link).
Result: `eth0` = PS PHY, `eth1` = PL PHY, both driven by the Linux `macb` driver.

## 6. Vivado on this PC — and why it is no longer on the critical path

The PL is built with the open toolchain and the PS with U-Boot SPL + `ps7/ps7_init_gpl.c`
([OPEN-TOOLCHAIN.md](OPEN-TOOLCHAIN.md), [PS-LINUX.md](PS-LINUX.md)). Vivado is still the only way
to get AMD's IP (GMII-to-RGMII for `eth1`, AXI DMA) and sign-off timing.

### Vivado status

Not installed (searched C:, D:, P:, registry, Start Menu, PATH — 2026-09-24). Installing needs
an AMD account to download the Unified Installer, and **C: has 43 GB free — too small**; install
to **D:** (619 GB free) or **P:** (186 GB), selecting only Vivado + Zynq-7000 (~60 GB). Until
then, `constraints/*.xdc` and `ps7_starlite.tcl` are checked by transcription against the
manual (`tools/check_xdc.py`), not by synthesis.
