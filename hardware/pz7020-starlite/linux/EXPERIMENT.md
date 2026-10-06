# The FCLK0 experiment: what is proven where

FPGA #1's first Linux boot (2026-10-05) never drew its dark-blue screen, and its last write to the card was
at 8.014 s of uptime. Linux had switched off FCLK0 at 1.91 s. That this stopped the board is inferred from
the card (`machine/cards/fpga1-first-boot-forensics.txt`). When it stopped, the card cannot say: an
emulated boot of the same image went 14.3 s without a commit while booting and 25.9 s once booted
(`machine/cards/validate_jbd2_method.sh`).
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
| The routed netlist (the checkpoint the bitstream was written from) clocks the counter from FCLK0 with nothing to gate it in the fabric | `vivado/check_routed.tcl` on `system_top_postroute_physopt.dcp` | the counter's 64 flip-flops and the PS7's GP0 clock pin are on `clk_fpga_0`, through a plain BUFG straight from the PS7's FCLKCLK; no BUFGCE in the design |
| FCLK0 is the same whatever the board's silicon revision | the three tables in `ps7_init_gpl.c` | `FPGA0_CLK_CTRL` = `0x00200500` in all three |
| `plx.py`'s own arithmetic, against a counter that moves | `test_plx.py` (a PC backend: the gate stops the counter / does not / no counter) and the driver's verdict code | 36/36: ~100 MHz measured; each world gets its verdict; the SLCR lock restored either way; a board that froze whole gets a verdict; the guard's log parsed in its real line format, with other console output spliced in; the port pattern takes the SPL's banner and not another board's chatter |
| The gate test does not depend on FCLK0 being exactly the 100 MHz the design was built for | `test_plx.py` with the counter at 95 MHz while `CLK_HZ` says 100; then the old arithmetic on the same input | the first version counted ticks at `CLK_HZ`: at 95 MHz the counter falls 0.1 s behind over the window by itself, and **both** answers came out "partial, not understood" (rerun on the old line: exactly that). It now counts at the rate the counter itself runs at just before and after the gate: STOPS and is WRONG come out right at 95 MHz |
| The stall verdict cannot mistake a late reader for a stalled read | `test_plx.py`; `plx.py gate` and `rdtimed` now print CLOCK_MONOTONIC moments, shared by every process on the board | the reader is a fresh `python3` started after the gate is set; if it only reached its read after the gate was cleared, the first version reported "did NOT return while the gate was set". Now: issued before the clear and returned after = stalled; issued after the clear = "no conclusion" |
| Step 1 records fclk0 in the kernel's clock tree | the QEMU captures of step 1 | the first version's `grep fclk | head -8` stopped at fclk3 and fclk2 (`clk_summary` lists them first) and never showed fclk0; it now greps fclk0 and fclk1 themselves |
| `plx.py` leaves the system-control registers as Linux needs them | QEMU: lock status after `plx.py`, then `reboot -f` | the first version LOCKED them (Linux unlocks once at boot and never again; its restart handler writes without unlocking): `SLCR_LOCKSTA 1`, "Reboot failed". Fixed: `0`, the reboot comes back |
| The driver end to end, every path short of silicon | `test_driver_e2e.sh`: emulated boards with the console on TCP (through the production U-Boot where the boot path matters), run in parallel; transcripts in `machine/zynq/regression-20261006/driver-e2e/` | 42/42 over 8 paths. (1) card #1's own files beside a noisy fake CH340: the board's port picked, the fake left alone, the gate Linux set found and cleared, the PL read returns, every step, the reboot captured, the three PL services active again (the screen would turn dark blue), the SLCR left unlocked, the record on the card read back. (2) the guard's read held forever: the missing report noticed 60 s after the login, one Enter finds the shell, the stuck read named, the PL never read again. (3) the board already up: one Enter after 90 s of silence. (4) the boot frozen: SILENT, exit 3. (5) no return after the reboot: the verdicts so far kept, exit 3. (6) no U-Boot: refused on the guard's problems. (7) the Pi check on the board's real nbd-server: 0 without a client, 1 with one. (8) the guard masked: the driver's own register reading refuses the PL alone |
| The driver records memory and the SLCR lock on the board | step 1 `free -m`, step 6 `plx.py lockstate` | in QEMU: 59 MB available with the swap export on; lock left unlocked |
| What the card can say about when a board stopped | `machine/cards/validate_jbd2_method.sh`: QEMU boots of the image, power cut at known moments, read exactly as card #1 was | the last commit lies before the cut, but a normal boot went 14.3 s without a commit, and 25.9 s once booted: the card bounds the stop from below only |
| The bring-up journald setting (`SyncIntervalSec=2s`) | `machine/cards/validate_journal_sync.sh`: a tick every 0.5 s to the console and the journal, power cut, the card read back; and the same without the setting | with it: the card 2.8 s behind the console; without it: 12.8 s |
| The boot chain from the SPL onward | `qemu_spl_chain_test.sh`: a test SPL (the board's U-Boot tree, ps7_init replaced by the board's final clock values) loads the production `u-boot.img`, `boot.scr`, `pl.bit`, kernel and root filesystem from the card image | SPL, U-Boot, `boot.scr`, Linux, autologin, report: all reached. It had printed nothing for weeks: the harness loaded the SPL's ELF, which carries no device tree (`CONFIG_OF_SEPARATE`), so `spl_init()` failed and it sat in `hang()` -- found from QEMU's monitor (PC, stack, addr2line); it now loads `u-boot-spl-dtb.bin` as the BootROM does |
| What changed on card #1 since the boot that stopped | the old card's files against today's image | device tree: only `fclk-enable` 0 -> 1; `boot.scr`: only `clk_ignore_unused` (+ a comment); first stage, U-Boot, kernel, bitstream byte-identical; root filesystem: the guard, its unit and drop-ins, the journald setting, `zynq-node` and its unit, `machine-bench` added; `zynq-report` (the guard's row) and a comment in `fpgagpud.service` changed; nothing else |
| The owner is told when the board froze or the run finished | the driver's beep and message box (the HDMI cannot show a frozen CPU: scanout runs on the GPU's own clock) | beep and a self-closing box tested on this PC; a box raised by a hidden, detached python (the way the armed driver runs) reached the desktop (it returned OK, not the timeout code). The PC never sleeps (both idle timers 0), so it cannot sleep through the boot |
| The screen stays dark blue while step 4 stops the GPU daemon | `accel/gpu/zynq/fpgagpud.c` | no signal handler, nothing written on exit: the GPU keeps scanning out the last frame; scanout runs on the GPU's own clock, not FCLK0 |
| Stopping FCLK0 for the gate test cannot leave another clock broken | `vivado/build/system_clocks.rpt` (the routed design's clocks); the device tree's PL nodes | the design's two MMCMs are fed by the 50 MHz oscillator (GPU) and FCLK1 (eth1); none by FCLK0, so no lock to lose. Every PL node is `generic-uio` with no interrupt: nothing in the kernel touches the fabric while the services are stopped |
| With the Pi switched off (eth0 without a link, which QEMU never has), nothing ahead of the report waits for the network | the image's units, read from the image | the report, the guard and the three PL services order only on `network.target`; the only unit before it, ifupdown's `networking.service`, configures `lo` alone (eth0 is systemd-networkd's, which does not hold `network.target`). So a dark eth0 cannot delay the report into the driver's 60 s check |
| The PC side: CH340 driver, real port | `pnputil`, COM1 opened as the driver does | WCH ch341ser 3.9.2024.9 installed; four CH340 boards seen on this PC before; DTR/RTS held low, reads time out cleanly |
| Xilinx's own definition of the gate bit | the Vivado install (processing_system7 VIP register files) | addresses and reset values only (`FPGA0_THR_CNT` resets to 0, `FPGA0_THR_STA` to `0x00010000`); no text says what the bits do |

## Only the board can answer

- **Does the gate bit stop FCLK0 in the silicon?** The PS7 is a hard block; no simulation has its clock
  gate. The gate test measures it: the counter is 2 s short if the bit stops the clock, on time if not.
- **Does a fabric read hang while FCLK0 is stopped, on the real PS's GP0 port?** The stall test.
- **Did the fix work?** A dark-blue screen, the guard's verdict OK, the PL's ID read back.
- **What FCLK0 really runs at** (step 3, against the board's own CPU clock; the gate test no longer assumes
  100 MHz) and **how much memory the board has free with the Pi's swap export on** (step 1; QEMU: 57 MB).

## Limits of what was checked here

- The clock-stop simulation drives `pl_regs` from a bus master on a free-running clock. On the board the
  PS7's own GP0 port runs on FCLK0 too (the routed netlist shows `MAXIGP0ACLK` on `clk_fpga_0`), so the
  PS's side of the bus stops as well. The simulation shows the fabric side never answers while the clock
  is held; what the PS's hard GP0 bridge does then is the board's stall test.
- QEMU has no fabric: every PL read there returns 0. The driver's verdicts were checked against
  `plx.py`'s PC backend instead (`test_plx.py`), not against silicon.
- QEMU's guest clock ran about 18% fast against this PC's clock in these runs; every comparison above is
  made on the guest's own clocks.

## Not covered

- No watchdog driver is enabled (`CONFIG_CADENCE_WATCHDOG` off): a hung board stays hung until power-cycled.
- Read, not run: the driver's 10-minute "nothing heard" message (it fires only for a real COM port that appears
  after the driver started; the tests use TCP ports), and the step-5 branch that skips the stall test when
  the Pi is attached (its check command ran on an emulated board, scenario 7; the branch around it did not).
- Expected, not observed: on silicon a CPU stuck in a bus read cannot pass through RCU, so the kernel should
  print stall warnings after 21 s (`CONFIG_RCU_CPU_STALL_TIMEOUT=21`). The driver does not rely on them.
- The bitstream also uses FCLK1 (200 MHz, timing met), only for the second Ethernet port's GMII-to-RGMII
  converter. GEM1 is disabled in the device tree and the first stage never enables FCLK1, so nothing at boot
  touches it. When the eth1 recipe (`machine/zynq/ETH1.md`) is applied, `fclk-enable` must become `<0x3>`.
- Vivado's timing report for this bitstream: all user constraints met; FCLK0 domain WNS +0.957 ns at 10 ns.
- `boot.scr`'s comment on card #1 says the board "froze at the first PL register read": an inference
  written as fact; comment only, corrected at the next image build like the one below.
- `zynq-plcheck`'s header comment still says the board "stopped between 8.0 s and about 13 s". That is wrong
  (see the top of this page and docs/61); the comment changes nothing the guard does. The file is kept
  byte-identical to card #1's copy, which `machine/cards/verify_cards.py` checks, and is corrected at the
  next image build. The same header says QEMU answers a PL read with a bus error; QEMU returns 0 (its
  serial logs: `0x40000000 = 0x00000000`). The guard handles both, so this too is comment only.
