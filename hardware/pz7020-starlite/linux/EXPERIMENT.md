# The FCLK0 experiment: what is proven where

FPGA #1's first Linux boot (2026-10-05) stopped between 8.0 and about 13 s of uptime. Linux had switched
off FCLK0 at 1.91 s. That cause is inferred from the card (`machine/cards/fpga1-first-boot-forensics.txt`).
`board_experiment.py` settles it on the board. This page says which part of the argument rests on what.

## Checked on this PC

| Claim | How | Result |
|---|---|---|
| The card's `pl.bit` is Vivado's `system.bit` | `vivado/check_xsa.py`: sha256 of the bitstream inside `system.xsa`, `vivado/build/system.bit`, `linux/out/pl.bit` | identical (`17b560d2…`) |
| `pl_regs` is at `0x40000000`, `CLK_HZ` = 100 MHz, clocked by FCLK0 | `check_xsa.py`, from Vivado's hardware handoff (`system.hwh`) | yes |
| The GP0 interconnect and the PS7's own `M_AXI_GP0_ACLK` run on FCLK0 | same | yes: with FCLK0 stopped, no register access to the fabric has a clock on either side |
| The SPL sets FCLK0 the way the design was built for | `check_xsa.py`: the SPL's final `FPGA0_CLK_CTRL` (`0x00200500`) against the design's source and dividers | IO PLL / 5 / 2 on both; the SPL source is identical to the U-Boot tree's |
| The registers `plx.py` reads behave as it assumes | `ps7-axi/run_xsim.ps1` (Vivado xsim, the bitstream's `pl_regs.v`) | ID, CLK_HZ, SCRATCH, TIME_HI latched by the TIME_LO read |
| With its clock held, the counter stops for exactly the held cycles | same, `tb_pl_regs_clockstop.v` | short by exactly 200, 1000, 5000 cycles |
| A read issued while the clock is held gets no answer, then completes | same | no response for 3000 cycles, done 3 cycles after the clock runs |
| Linux writes FCLK0's gate bit with card #1's device tree | `qemu_plcheck_test.sh` (QEMU keeps the SLCR registers Linux writes) | `FPGA0_THR_CNT = 1`, fclk0 enable count 0; today's files: 0 and 1 |
| The guard blocks on that bit and the board stays up | same | BLOCKED on FCLK0 alone, services held back, report printed |
| The driver: boot watch, board identification, transfer, every step, verdict | `board_experiment.py` against QEMU boots, with a fake noisy CH340 beside it | picks the board, sends the fake nothing, all steps run |
| The driver survives a frozen board and a power cycle | `--test-reboot`: QEMU halts, the harness kills and restarts it | the driver reopens the port and captures the new boot |
| The driver refuses to read the fabric when the guard found another problem | QEMU boot without U-Boot (level shifters off, fabric in reset) | no fabric read |

## Only the board can answer

- **Does the gate bit stop FCLK0 in the silicon?** The PS7 is a hard block; no simulation has its clock
  gate. The gate test measures it: the counter is 2 s short if the bit stops the clock, on time if not.
- **Does a fabric read hang while FCLK0 is stopped, on the real PS's GP0 port?** The stall test.
- **Did the fix work?** A dark-blue screen, the guard's verdict OK, the PL's ID read back.

## Not covered

- `qemu_spl_chain_test.sh` prints nothing in QEMU (the test SPL never hands over to U-Boot there). The same
  SPL bytes ran on the board on 2026-10-05, so this is a gap in the emulation, not in the card.
- No watchdog driver is enabled (`CONFIG_CADENCE_WATCHDOG` off): a hung board stays hung until power-cycled.
