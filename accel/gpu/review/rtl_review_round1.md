# RTL review, round 1 (adversarial, read-only)

Scope: `rtl/scanout.v`, `rtl/video_reader.v`, `rtl/video_pixel.v`, `rtl/video_serializer.v`,
`rtl/async_fifo.v`, `rtl/tmds_encoder.v`, `rtl/par_rx.v`, `rtl/axi_gp_regs.v`, `rtl/sync_fifo.v`,
`rtl/clkgen.v`, `rtl/gpu_top.v`. I also skimmed `rtl/gpu_core.v` and `rtl/core_*.v`, and read
`fpga/pz7020_gpu.xdc` and `fpga/build.tcl` where they touch these modules.
Reference documents: `SPEC.md` (the version modified 18:32, where the core clock is 148.75 MHz),
`common/gpu_proto.h`, `rtl/INTERFACES.md`, `rtl/gpu_defs.vh`.
I reviewed the RTL files as they were at 17:49–18:24. None of them changed during the review.

Method: I read every listed file line by line against the hunt list (CDC, AXI3, OSERDESE2, TMDS,
timing, FIFOs, widths, blocking/non-blocking assignments, latches, resets, register map, par_rx
sampling, BUSY). I tried to refute every suspicion by re-reading the code path, and where a
simulation could settle the question I wrote a small experiment under `review/experiments/`.
I only report findings I'm confident about. Where a finding depends on something the RTL can't
determine (for example board signal integrity), I mark it UNCERTAIN.

Overall: the scanout, TMDS, OSERDES, AXI and FIFO logic is solid. I found **no functional RTL bug**
in the listed files. The findings below are one robustness weakness whose failure mode I
confirmed in simulation, one mismatch between the spec and the RTL/build files, one constraint
gap, and two informational items.

## Findings

| ID | Sev | Status | Where | One line |
|---|---|---|---|---|
| R1-01 | Medium | Consequence CONFIRMED (sim); trigger UNCERTAIN (board SI) | `rtl/par_rx.v:59` | Any sampled STROBE glitch inserts a word, so the rest of the record shifts by one word and the record is still accepted and rendered |
| R1-02 | Medium | CONFIRMED (mismatch) | `rtl/clkgen.v:39` + 6 other places | SPEC now says core = 148.75 MHz (÷5); RTL, build and constraints still say 106.25 MHz (÷7) |
| R1-03 | Low | CONFIRMED (constraint gap); practical impact UNCERTAIN | `fpga/pz7020_gpu.xdc:67-69` | `set_clock_groups -asynchronous` leaves the async FIFO's 11-bit gray pointer buses with no skew or max-delay bound |
| R1-04 | Info | CONFIRMED | `rtl/gpu_top.v:90,346` | SOFT_RESET clears T_WORDS / TEENSY_ACTIVE and pulses BUSY for 1 cycle; the collector's counters survive |
| R1-05 | Info | CONFIRMED | `rtl/gpu_defs.vh`, `rtl/axi_gp_regs.v:40-59` | RET_* register offsets exist only as `ifndef` fallbacks in axi_gp_regs.v, not in gpu_defs.vh |

---

### R1-01 — par_rx has no STROBE glitch rejection; one glitch silently shifts a record (Medium)

**Where:** `rtl/par_rx.v:59` (`wire xfer = stb_s2 ^ stb_s3;`) and the assembler at `rtl/par_rx.v:81-92`.
The collector accepts the result without any check: `rtl/core_collector.v:264` closes the record
on its 24th word, whatever that word is.

**What breaks:** The bus is toggle-strobed, so every level change of the synchronised STROBE
counts as one transfer. A glitch (away from the level and back) always adds an **even** number
of edges, so the half-word phase survives and nothing looks misaligned. The glitch still adds
one extra 32-bit word, built from a duplicated half. Every later word of that record moves one
position, and the collector closes the record after 24 words. The shifted record is accepted
and rendered: for a TRI, the edge, z and colour coefficients all land in the wrong fields. The
record's real last word (w23) then arrives with sor=0 while no record is open. It is discarded
and counted as one BAD_RECORDS. That counter is the only trace.

**Concrete failure scenario:** In autonomous mode (SPEC 14), about 1500 TRIs/frame × 48 edges ×
60 fps ≈ 4.3 M STROBE edges/s. JM1 has only 6 GND pins, all at the ends of the header (3,4 and
33..36), while 16 data lines switch in the same instant. D15 is on pin 24, two pins from STROBE
on pin 26. With a single-ended ribbon or jumper cable, ringback or crosstalk crossing the 1.5 V
threshold for a few ns is plausible. Even one sampled glitch per 10^7 edges means a garbage
triangle about every 2 s: random edge coefficients, often with a full-screen bbox, flashing on
the display. Evidence would be BAD_RECORDS creeping up while the Teensy reports no aborted
records.

**Experiment:** `review/experiments/tb_par_rx_glitch.v`, run at both core clocks. Clean
traffic at the SPEC minimum (30 ns setup, 60 ns hold) with random clock phase: rtl/par_rx
50/50 exact. With one glitch injected per record:

| glitch | rtl/par_rx corrupted records @148.75 MHz | @106.25 MHz | filtered proposal (both clocks) |
|---|---|---|---|
| crosstalk 1 ns | 5/40 | 7/40 | 0/40 |
| crosstalk 3 ns | 14/40 | 14/40 | 0/40 |
| crosstalk 6 ns | 37/40 | 26/40 | 0/40 |
| crosstalk 12 ns | 40/40 | 40/40 | 0/40 |
| ringback 0.5–2.5 ns after the edge, 6–12 ns wide | 7–11/40 | 0–8/40 | 0/40 |

Every corrupted case pushed 49 words for 48 expected, with the pattern the analysis predicts.
Example: `first bad word 6: got 0005c005c expected 004a3005c; next got 004a3005c`. That is one
duplicated half-word, then the correct stream shifted by one word. The probability rises with
glitch width (width divided by the clock period, as expected for an unfiltered sampler). A
faster core clock catches more narrow glitches.

**Refutation attempts:**
- Could the phase logic catch it? No. The number of added edges is always even.
- Does SOR re-alignment help? Only for the next record.
- Does a glitch on transfer 0 corrupt data? No. SOR=1 appears twice and the partial record is
  discarded, so that case self-heals.

The mechanism is certain. Whether the real cable glitches is not something the RTL can decide,
so the trigger is UNCERTAIN.

**Suggested fix (the second item is optional):**
1. Add a digital glitch filter: accept a new STROBE level only after it has been stable for 3
   consecutive samples. Take D/SOR from `d_s2`/`sor_s2` in the accepting cycle. That is D
   sampled at most about 3 clocks after the edge, so ≤ 28 ns at 106.25 MHz or ≤ 20 ns at
   148.75 MHz, well inside the 60 ns hold. A drop-in version is in
   `review/experiments/par_rx_filt.v` (same ports; about 3 more flops). It can never accept a
   pulse shorter than 2 core periods (13.4 ns at 148.75 MHz, 18.8 ns at 106.25 MHz): 0
   corruptions in every case above, and clean 30/60 ns traffic stays exact.
   SPEC 3's "same pipeline age" receiver sentence and `sim/tb_par_rx.v` phase H would need an
   update. Latency grows by 2 clocks, which BUSY's 40-word margin easily absorbs.
2. Weak but free second line of defence in the collector: SPEC and gpu_proto.h say w23 = 0
   for every record type. Reject a record whose 24th word is non-zero and count it in
   BAD_RECORDS. This only catches the shift when the original w22 (dbdy) is non-zero, i.e.
   Gouraud TRIs with a vertical blue gradient. It misses flat-coloured TRIs, rects (all
   gradients 0), SPRITE and END records. A real per-record check word (for example a CRC in
   w23) would be robust, but it is a protocol change (Teensy firmware, gpu_setup.c, daemon,
   golden model).
3. Board: add a 22–33 Ω series resistor at the Teensy STROBE output. Run a GND wire next to
   STROBE, or use the GND pins closest to it.

---

### R1-02 — Core clock: SPEC says 148.75 MHz, RTL/build/constraints say 106.25 MHz (Medium)

`SPEC.md` §2 (changed at 18:32) now specifies `CLKOUT0_DIVIDE_F 5 -> core 148.75 MHz`. Nothing
else followed:

| Place | Current | Consequence if left / if changed carelessly |
|---|---|---|
| `rtl/clkgen.v:39` | `CLKOUT0_DIVIDE_F (7.0)` → 106.25 MHz (header comment line 5 too) | Design runs at 106.25 MHz, not the SPEC clock |
| `fpga/build.tcl:40` | `core_hz 106250000` → FREQ_HZ of GP0/HP0/HP1/HP2 ACLK ports | If only clkgen changes, the BD metadata is wrong (no hardware effect, confusing) |
| `rtl/par_rx.v:25` | `ACTIVE_CYCLES = 10625000` (100 ms at 106.25 MHz) | At 148.75 MHz TEENSY_ACTIVE covers 71 ms. The correct value 14,875,000 still fits 24 bits |
| `rtl/gpu_top.v:581` | `localparam [25:0] HB_HALF = 26'd53124999` | At 148.75 MHz the heartbeat is 1.4 Hz. **Trap:** the correct value 74,374,999 does not fit 26 bits. iverilog only warns and uses 7,266,135 (≈10 Hz blink). Verilator `-Wall` does flag it (so `sim/lint.sh` would fail). Widen `hb_cnt`/`HB_HALF` to 27 bits (`review/experiments/trunc_check.v`) |
| `fpga/pz7020_gpu.xdc:62`, `rtl/scanout.v:19`, `rtl/async_fifo.v:9` | comments say 106.25 MHz / "set_max_delay 9.4" | 9.4 ns is no longer "≤ one period of the faster clock" at 6.72 ns |
| `sim/tb_par_rx.v` (TCLK), `sim/tb_scanout.v` (default core half period 4.706 ns) | 106.25 MHz | Default regressions don't run at the SPEC clock. tb_scanout does sweep 3.0 and 6.0 ns |

Fix: change all of these together in one step, then re-run full implementation timing at 6.72 ns.
The OOC figure quoted in SPEC is synthesis-only. The raster lane adders plus clamps plus the Z
compare in `core_tri` S1/S2 are the obvious paths to watch. Or revert SPEC §2. Either way, one
owner should decide.

---

### R1-03 — Gray-pointer CDC is unconstrained (`set_clock_groups -asynchronous`) (Low)

**Where:** `fpga/pz7020_gpu.xdc:67-69` cuts all core↔video paths. The multi-bit crossings are the
async FIFO pointers: `rtl/async_fifo.v:43` `wgray` → `:79` `wgray_s1`, and `rgray` (`:78`) →
`rgray_s1` (`:44`). Both are 11 bits.

**What breaks:** A gray-coded bus is only safe if the skew between its bits, from the source
register to the ASYNC_REG destination, stays below one destination clock period. With the
clock-groups exception Vivado applies no constraint at all. In a congested placement the
source register (placed with the FIFO logic) can end up far from the synchroniser, and two
bits can arrive in different destination cycles.

**Failure scenario:** The sampled `wgray` is off by more than one step. `wbin_r` then jumps
ahead, the read side "sees" words that were never written, and it pops stale RAM entries.
The result is a burst of wrong pixels plus a false "unexpected sof", then a resync one frame
later. On the write side, a wrong `rbin_w` corrupts `wcount`, and the reader's space
reservation could then overflow the FIFO.

Vivado's `report_cdc` will list these as CDC-6 (multi-bit bus through ASYNC_REG). Practical risk
is low because the routes are short, but nothing guarantees it.

**Fix:** Add `set_bus_skew` on both pointer buses, for example
`set_bus_skew -from [get_cells -hier -filter {NAME=~*u_scan/u_fifo/wgray_reg*}] -to [get_cells -hier -filter {NAME=~*u_scan/u_fifo/wgray_s1_reg*}] 6.0`,
and the same from `rgray_reg*` to `rgray_s1_reg*`. Check with `report_bus_skew` and `report_cdc`.
Alternatively, drop the blanket clock groups and put `set_max_delay -datapath_only` on the CDC
paths. Please check which exception takes precedence in 2026.1: set_clock_groups overrides
set_max_delay, while my understanding is that bus-skew constraints are not overridden.

**Not a problem:** the single-bit crossings (`scanout_en` → `en_s1`, `vs_toggle` → `vt_s1`,
hpd/locked) are fine under clock groups. Each has a register source and an ASYNC_REG
destination.

---

### R1-04 — SOFT_RESET resets par_rx statistics and pulses BUSY (Info)

`rtl/gpu_top.v:90,346`: par_rx sits on `rst_io = rst | soft_reset`. Every soft reset therefore:

- zeroes T_WORDS (`words_rx`);
- drops TEENSY_ACTIVE for up to 100 ms;
- drives `tb_busy` high for one core cycle.

The daemon issues a soft reset on every new controller connection. The collector deliberately
keeps its counters across a soft reset (`rtl/core_collector.v` header). SPEC 7 only says
"resets bus receiver", so this is consistent, but the two counter policies differ.

No software reads T_WORDS or TEENSY_ACTIVE yet (grep over *.c/*.h/*.py). The BUSY pulse is
6.7–9.4 ns and harmless unless the Teensy is in its 10 ms arming window, which then restarts.

Decide the policy explicitly. If T_WORDS should survive, move `words_rx`/`act_cnt` to `rst` only.

---

### R1-05 — RET_* offsets missing from `rtl/gpu_defs.vh` (Info)

`rtl/axi_gp_regs.v:40-59` supplies `ifndef` fallbacks for `R_RET_ADDR..R_LAST_FRAME_NO` and
`RET_ADDR_RESET`. They match gpu_proto.h: 0x04C, 0x050, 0x054, 0x058, 0x05C and 0x1FE00000.
But if someone later adds them to gpu_defs.vh with a typo, the fallback silently hides the
disagreement. Add them to gpu_defs.vh and remove the fallbacks, or turn each fallback into a
compile-time check. rtl/INTERFACES.md also still lacks the RET ports and the `core_top`-instead-
of-`gpu_core` instantiation. Both are documented in the file headers.

---

## Hunted and refuted (checked, not bugs)

**CDC / reset**
- async_fifo:
  - pointers are registered gray codes (one bit changes per increment) through 2-FF ASYNC_REG
    synchronisers;
  - gray→binary conversion is correct;
  - `wcount` never under-estimates occupancy;
  - no BRAM read/write address collision: a slot is read at least 2–3 rclk after its write
    became visible, and rewritten only after the read-pointer update crossed back.
  - Resets released at different times are safe: a side held in reset presents pointer 0, which
    is its true pointer. Independent stress test: 100k words each at 5 clock ratios (write
    faster, read faster, near-equal, 4:1), AW=4 so full/empty/wrap are hit constantly. 0 errors,
    order intact, `wcount` ≥ RAM occupancy on every cycle, FWFT head stable (`tb_async_fifo_stress.v`).
- `scanout_en`, `vs_toggle`, hpd, locked: single-bit, register-launched, ASYNC_REG 2-FF.
  `vsync_count` edge detect uses s2^s3. No spurious count at start-up: vs_toggle first toggles
  about 110 µs after `rst_pix`.
- clkgen: per-domain 2-FF LOCKED synchronisers, INIT=1 reset registers, released 16 cycles after
  LOCKED. OSERDES reset `oser_rst_sr` is in clk_pix (= CLKDIV), INIT=1, one register shared by
  all 8 OSERDESE2 so they leave reset in the same CLKDIV cycle.
- par_rx: D/SOR/STROBE are sampled at the same pipeline age (`d_s2` with the `stb_s2` edge). The
  30 ns setup is ≥ 3 core clocks at both frequencies, so a metastable STROBE sample only moves
  the capture by one clock, still within the 60 ns hold. The synchronisers are deliberately not
  reset. `tb_*` pins have PULLDOWN in the XDC, so a floating Teensy bus doesn't strobe. Clean
  traffic at exactly 30/60 ns with random phase is exact at 106.25 and 148.75 MHz (R1-01
  experiment, phase 0).

**AXI3**
- video_reader (HP0):
  - ARVALID is a register;
  - ARADDR only changes when `!arvalid || arready`;
  - 128-byte aligned 16-beat bursts, so no 4 KB crossing (fb registers force 4 KB alignment);
  - RREADY=1 is safe because space is reserved before the AR (`fifo_count + reserved + 16 ≤ depth`, both terms conservative);
  - every beat's RRESP is counted;
  - the swap is granted only with zero reads outstanding.
- core_writer (HP1):
  - W never precedes its AW (`w_credit`);
  - WVALID can't drop before its handshake (`fq_v`, credit only falls on WLAST);
  - WDATA comes from a register FWFT head;
  - AWADDR is stable while waiting;
  - WLAST on beat 15; WID = AWID = 0; BREADY=1;
  - BRESP errors counted in core_top;
  - frame_done needs all B responses (14400, or 28800 with capture).
- core_sprite (HP2): bursts split at 4 KB (`to4k`); ARADDR/ARLEN are set only while ARVALID=0.
- axi_gp_regs (GP0):
  - AWREADY doesn't wait for W and WREADY waits for AW, so AW-first, W-first and simultaneous
    arrival all complete with no deadlock;
  - BID/RID echo the latched IDs;
  - OKAY responses; RLAST on beat ARLEN;
  - B/R payloads are registers held until READY;
  - FIXED/INCR/WRAP beat addressing; one register access per beat; WSTRB honoured.
  - Register map checked field by field against gpu_proto.h: every offset 0x000–0x05C, 0x100
    and 0x104; CONTROL reset 0x4 (SRC_PS); STATUS bits 0..6 in proto order; FB0/FB1/RET_ADDR
    reset values; PS_FIFO_FREE and T_FIFO_LEVEL widths; RET_ACK a self-clearing pulse.
  - WID is ignored. That is correct, because the PS7 GP0 master doesn't interleave and the slave
    takes one burst at a time.

**OSERDESE2 / TMDS**
- Master D1..D8 = d[0..7] with D1 sent first; slave D3/D4 = d[8]/d[9]; slave SHIFTOUT1/2 →
  master SHIFTIN1/2 (UG471 width expansion).
- DDR, DATA_WIDTH 10, TQ SDR with TRISTATE_WIDTH 1, OCE=1.
- CLK/CLKDIV come from the same MMCM on BUFGs; 743.75 Mb/s is within HR-bank limits at -2.
- Clock lane 0000011111 LSB-first. Lanes: [0]=blue+{vsync,hsync}, [1]=green, [2]=red, [3]=clock.
- tmds_encoder matches DVI 1.0 fig. 3-5 exactly. **Exhaustive check** with an independent
  reference: the reachable running disparity is {−8,−6,…,+8}, and 5-bit two's complement holds
  it. All 9 states × 256 data inputs plus all 4 control tokens from every state give 4420
  symbols compared, 0 mismatches (`tmds_exhaustive.py` + `tb_tmds_exh.v`). The control tokens
  equal the DVI table (q_out[0:9] written LSB-first) and SPEC 8.

**Video timing** (`rtl/video_pixel.v`)
- 1650×750 total; HSYNC high for pixels [1390,1430); VSYNC toggles with the HSYNC leading edge
  from line 725 to line 730 (CEA progressive convention).
- DE/HSYNC/VSYNC/RGB are all 2 registers after the counters and share the encoders' 2-cycle
  latency.
- The word for pixels 4k..4k+3 is popped at hc=4k and `px` is selected with `hc1[1:0]`, so
  there's no lane skew.
- RGB565→888 matches SPEC 8 and SPEC 4.
- SOF resync: first0 pops sof; underflow or unexpected sof → black until the next line 0;
  seek-discard is limited to non-sof words; synced is cleared at the start of vblank.
- No latches: every `always @*` has defaults.
- The FIFO is consumed identically in pattern mode, so swaps and FRAME_COUNT work with colour
  bars.

**FIFOs / widths / BUSY**
- sync_fifo:
  - `ram_cnt` can't reach 2^AW, so there's no read/write address collision;
  - count/free/full are registered and consistent;
  - FWFT output holds until popped.
- BUSY = registered(`rst || free < 64`), initial 1. Worst-case staleness: the last word of the
  previous record may not yet be counted when the Teensy samples BUSY (about 6 clocks of
  pipeline). The real guarantee is therefore ≥ 63 free rather than ≥ 64, which is harmless
  because a record needs 24.
- Width checks: video_reader reservation arithmetic is FIFO_AW+2 bits; `NB24`; core_writer
  15-bit burst counters (28800 < 32768); PS FIFO free is 10 bits for depth 512; t_level is 11
  bits; `{c_sel,cnt}` side address is 12 bits. The only truncation found is the HB_HALF trap
  in R1-02.
- Blocking assignments appear only in functions and `always @*`. No blocking assignments in
  sequential blocks.

**Swap / core handshake**
- `core_frame` samples `front_idx` in F_IDLE, after the previous swap. `do_swap` needs
  `!in_frame` plus `swap_armed`, where swap_armed means swap_req was seen low since the last
  swap. swap_req only drops after swap_done.
- The raster writes only bank `strip&1`, and only once strip−2 has been read and cleared. The
  writer's clears go to the other bank, so the clear-priority write mux in core_top never drops
  a raster write.
- Collector soft reset keeps only the list being rendered; `r_avail` is gated by soft_reset.

## Experiments (all under `review/experiments/`, outputs in `review/experiments/out/`)

Run through `wsl.exe -d Ubuntu-22.04 -- bash -s < script` (iverilog 11 / verilator 4.038).

| Experiment | Command | Result |
|---|---|---|
| Full-design lint | `bash sim/lint.sh` | LINT PASS (6 runs, full gpu_top with `-Og`) |
| TMDS exhaustive | `python3 tmds_exhaustive.py gen out/tmds_vec.txt`; `iverilog tb_tmds_exh.v rtl/tmds_encoder.v`; `vvp`; `python3 tmds_exhaustive.py check …` | 4420 symbols, 0 mismatches |
| par_rx glitch | `iverilog -I rtl tb_par_rx_glitch.v par_rx_filt.v rtl/par_rx.v`; `vvp +half=3361` and `+half=4706` | Table in R1-01 |
| async FIFO stress | `iverilog tb_async_fifo_stress.v rtl/async_fifo.v`; 5 × `vvp +wps=.. +rps=..` | 5/5 PASS |
| 26-bit truncation | `verilator --lint-only -Wall trunc_check.v`; iverilog | Verilator flags it; iverilog warns and elaborates 7,266,135 |
| Team scanout regression (re-run, output to `out/scanout_rerun`) | `OUT=… bash sim/run_scanout.sh small` | 5/5 PASS (small s1..s4 at core half periods 4.706/3.0/6.0/4.706 ns, and med 256x128). HD target and `sim/run_io.sh` not re-run (run_io writes to /tmp) |

## Not verified

- Anything on hardware: signal integrity of the JM1 bus (R1-01's trigger), HDMI eye, MMCM
  jitter.
- Timing closure at either core clock. I didn't run Vivado; building is the build owner's job.
- `report_cdc` / `report_bus_skew` output (R1-03).
- PS-side settings that U-Boot's own ps7_init controls. Worth one check at bring-up: the AFI
  HP0–HP2 `RDCHAN_CTRL`/`WRCHAN_CTRL` bit 0 (32BitEn) must be 0 for 64-bit ports (addresses
  from memory: 0xF8008000/0xF8008014 for AFI0, +0x1000 per port, so please confirm in the Zynq
  TRM). The PL→PS level shifters (SLCR LVL_SHFTR_EN = 0xF) must be enabled after the bitstream
  loads.
- core_* internals were skimmed, not exhaustively reviewed; the team's golden-model compare
  covers them. Things I did check: operand capture at start in core_tri, the clear/raster bank
  ownership, AXI rules in core_writer and core_sprite, and collector soft reset.
