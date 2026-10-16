# eth1 — the FPGA board's second Ethernet port (phase 2)

Each PZ7020-StarLite has two gigabit jacks. The upper one (ETH-PS, `eth0`) is the PS's own GEM0 on
MIO and works today. The lower one (ETH-PL, `eth1`) is GEM1 routed through the PL: GEM1 over EMIO →
the GMII-to-RGMII core in the bitstream → the RTL8211F at PHY address 2 on bank 34. The bitstream
already carries the core (`hardware/pz7020-starlite/vivado/build_system.tcl`) and the device tree
already describes it (`linux/zynq-pz7020-starlite.dts`, `&gem1 … status = "disabled"`). What is
missing is **clocks**: the 16-bit PS configuration that boots this board leaves GEM1 and FCLK1 unclocked,
and the 32-bit configuration that did clock them is the one that was silent on the board
([[untested-first-stage-killed-the-boot]]). So eth1 is off, by choice, until it can be switched on
without touching the first stage.

## The recipe: clocks from U-Boot, not from the SPL

The five register writes that the 32-bit set made and the 16-bit set does not, lifted from
`ps7-vivado/ps7_init_gpl.c` (lines 293, 321, 395, 456) against `ps7/ps7_init_gpl.c` (line 423). Both
sets run the IO PLL at 1000 MHz (`0xF8000108[18:12] = 0x1E` in both), so the divisors carry over
unchanged:

| register | name | value | meaning |
|---|---|---|---|
| `0xF8000008` | SLCR_UNLOCK | `0x0000DF0D` | unlock the SLCR |
| `0xF800013C` | GEM1_RCLK_CTRL | `0x00000011` | GEM1 receive clock from EMIO (the PL core), active |
| `0xF8000144` | GEM1_CLK_CTRL | `0x00100141` | GEM1 transmit clock from EMIO (SRCSEL = 100), DIV0 = DIV1 = 1, active |
| `0xF8000180` | FPGA1_CLK_CTRL | `0x00100500` | FCLK1 = IO PLL ÷ 5 ÷ 1 = **200 MHz**, the GMII-to-RGMII core's IDELAYCTRL reference |
| `0xF800012C` | APER_CLK_CTRL | `0x01DC04CD` | the 16-bit set's `0x01DC044D` plus bit 7: GEM1's AMBA clock |
| `0xF8000004` | SLCR_LOCK | `0x0000767B` | lock again |

Done in `boot.cmd` (a file on the BOOT partition, compiled to `boot.scr`) **before `bootz`**, guarded by
a flag file so a card without it boots exactly as today:

```
if test -e mmc 0:1 eth1; then
    echo "eth1: clocking GEM1 and FCLK1 (machine/zynq/ETH1.md)"
    mw.l 0xF8000008 0x0000DF0D
    mw.l 0xF800013C 0x00000011
    mw.l 0xF8000144 0x00100141
    mw.l 0xF8000180 0x00100500
    mw.l 0xF800012C 0x01DC04CD
    mw.l 0xF8000004 0x0000767B
    setenv bootargs "${bootargs} clk_ignore_unused"
    setenv fdtfile zynq-pz7020-starlite-eth1.dtb
fi
```

`clk_ignore_unused` keeps Linux's clock framework from gating FCLK1 late in boot (nothing in the device
tree claims it; the GMII-to-RGMII driver takes no clock). `zynq-pz7020-starlite-eth1.dtb` is the same
tree with `&gem1 { status = "okay"; }` — a second DTB beside the first, never replacing it. The SPL,
U-Boot, kernel and bitstream are the tested files, byte for byte.

## Why this is safe enough, and what it is not

- Nothing in the first stage changes: no ps7 table, no DDR. The board boots to U-Boot as it does now.
- The writes happen after DDR is up and U-Boot is running; a wrong value here cannot produce the silent
  board of 2026-09-26. The worst case is a kernel that fails to bring `eth1` up, or hangs probing it,
  and the recovery is deleting the `eth1` flag file from the card on any PC.
- It is still an untested change to the boot files, so it is done **on FPGA #1 first, from its console,
  after FPGA #1's first full boot has been reported**, and only then on FPGA #2.

## The gate

`ip link show eth1` reports the link up with the P4 (or a laptop) on the lower jack; `ping` both ways;
`ethtool eth1` shows 100 or 1000 Mb/s. Then the P4 cable (cable 9) is the panel's path.

## Also possible later

With FCLK1 and GEM1 alive, the same recipe gives FPGA #1's lower jack a second link — a direct cable to
the PC's future USB-Ethernet adapter, or to a third board — without a switch.
