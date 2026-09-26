# Zynq XC7Z020 (PZ7020-StarLite) — what it can and can't do for this project

Companion to [README.md](README.md) (board/pinout) and
[BOOT-SD-runbook.md](BOOT-SD-runbook.md) (getting it to boot).

**Confidence marks:** ✅ multi-source / primary · ⚠️ single source · 📐 my calculation, stated
assumptions, **not** a benchmark.

---

## 1. What's actually on the chip

| Resource | Amount | Conf |
|---|---|---|
| PL logic cells | 85 K (53,200 LUT / 106,400 FF) | ✅ |
| **DSP48 slices** | **220** | ✅ |
| **Block RAM** | **4.9 Mb ≈ 613 KB** | ✅ |
| PS CPU | **2× Cortex-A9 @ 766 MHz** | ✅ |
| DDR3 (this board) | 512 MB (one x16 MT41K256M16, 16-bit bus) | ✅ schematic + datasheet + chip marking |
| **Hardware video codec** | **NONE** — Zynq-7000 has no VCU (that's UltraScale+ **EV**) | ✅ |

### The BRAM number is the one that decides things
613 KB of on-chip RAM is small. Anything needing a frame buffer goes to DDR3 behind a custom
memory pipeline. Worked example: a single **720p YUV420 video frame is 1.32 MB** — more than
double *all* the BRAM on the chip. That one fact rules out fabric video decode (see §4).

---

## 2. How the ARM (PS) and fabric (PL) talk

Three AXI interface classes ✅:

| Port | Width | Purpose |
|---|---|---|
| **GP** (general purpose) | 32-bit | Register/control plane. PS→PL (`M_AXI_GP`) and PL→PS (`S_AXI_GP`). |
| **HP** (high performance) | 64 or 128-bit | DMA-style bulk movement between PL and DDR/OCM. |
| **ACP** (accelerator coherency) | 64-bit | Cache-**coherent** access from PL into the PS caches. |

**Measured throughput: >1.6 GB/s full-duplex** on HP and ACP at 125 MHz on an XC7Z020-1C ⚠️.

Practical pattern: control/config over **GP**, sample streams over **HP** into DDR, and **ACP**
only when the CPU must see data without a cache flush.

> ⚠️ Exact port *counts* (commonly cited as 2× M_GP, 2× S_GP, 4× S_HP, 1× ACP) are **not
> confirmed here** — read the Zynq-7000 TRM (UG585) before committing a block design.

---

## 3. Toolchain — free, and it covers this chip ✅
- **Vivado ML Standard Edition** (formerly WebPACK) is **free, no license file, not a trial**.
- **XC7Z020 is supported** by that free edition (it's the ZedBoard part, long WebPACK-covered).
- Reported free for commercial use as well ⚠️.
- **Install size: ~60 GB if you select only the Zynq-7000 family**; 100+ GB if you let it
  install everything. **Select only Zynq-7000.**
- The repo's OSS CAD suite (yosys/nextpnr) **cannot** target Zynq — iCE40/ECP5 only. Zynq work
  is Vivado. RTL can still be *simulated* with iverilog without Vivado.

---

## 4. What the PL should and shouldn't do here

### ✅ Audio front-end — the strongest case
The single biggest obstacle to "hey car" working at highway speed is **noise**: road, wind,
HVAC, and the system's own music playing through the speakers it's listening over. Fixed-latency
multichannel DSP is exactly what fabric is for.

**📐 Resource calculation** (assumptions stated, not benchmarked):
- AEC as 512-tap NLMS at 16 kHz voice rate: 512 MAC to filter + 512 to update
  = 1024 MAC/sample × 16,000 samples/s ≈ **16.4 M MAC/s**.
- One DSP48 at 100 MHz ≈ **100 M MAC/s** (1 MAC/cycle), and it can clock far higher.
- → AEC ≈ **0.2 of one DSP48 slice**.
- A 4-mic adaptive beamformer (4 × 128 taps, NLMS) lands in the same order (~16 M MAC/s).
- 512-point FFT noise suppression at ~62 frames/s is smaller still.

**Conclusion: the whole voice front-end plausibly fits in well under 10 of the 220 DSP slices.**
Even allowing an order of magnitude for control logic and buffering, it's a rounding error on
this chip. *This is arithmetic, not a measurement — validate in simulation before committing.*

### ✅ Also well-suited
- **Hardware timestamping** of CAN/sensor events at ns resolution (see
  [car-system-architecture.md](../car-system-architecture.md) §2 — the FPGA as time master).
- **Deterministic multi-channel sampling** at exact intervals — Linux fundamentally can't.
- **Line-rate data reduction**: decimate/filter/gate a firehose so the Pi only sees what matters.
- **SDR front-end** (FFT/DDC) if RF work returns.

### ❌ Video decode — ruled out, with reasons
1. **No codec block** in Zynq-7000.
2. **One 720p frame (1.32 MB) > entire BRAM (613 KB)** — everything lands in DDR3 behind a
   custom pipeline. Production H.264 decoders are commercial, multi-year IP.
3. **The PS is slower than the Pi**: 2× A9 @ 766 MHz vs 2× A76 @ 2.0 GHz. Moving playback here
   makes it *worse*.

Decode stays on the Orange Pi's CPU. Note that resolution ceiling is **unmeasured** — see the
correction in [car-system-architecture.md](../car-system-architecture.md) §1b; don't design
around an invented limit.

---

## 5. What we can do now vs what's blocked
**The vendor bundle arrived 2026-09-24** (manual, schematic, pin/length xlsx, datasheets — the
course-demo RAR is truncated, see SYSTEM-INTEGRATION.md §5). What it unblocked:
- `.xdc` with every documented ball: [constraints/pz7020_starlite_board.xdc](constraints/pz7020_starlite_board.xdc);
  the RTL-SDR accelerator's `sdr_accel.xdc` has real pins (JM1 9/11/13/15, U18, R19/V13).
- Fan pins H16/H17 confirmed (owner repo `fan_jm1.xdc`).
- PS7 configuration from the schematic straps and MIO map: [PS-CONFIG.md](PS-CONFIG.md) +
  [constraints/ps7_starlite.tcl](constraints/ps7_starlite.tcl). One open physical check: DDR chip count.

**Still blocked: Vivado is not installed on this PC** (C: too small; install to D:/P:). Nothing below
has been synthesised; utilisation and timing remain 📐 until it is.

**Doable now, without Vivado:**
- Write and **simulate** RTL with iverilog (AEC, beamformer, FFT, timestamping).
- Install Vivado (Zynq-7000 only, ~60 GB) so it's ready.
- Design the block architecture: which blocks sit on GP vs HP vs ACP.

---

## 6. Open questions
- [ ] Exact PS-PL port counts + widths — confirm in **UG585** (Zynq-7000 TRM).
- [x] PL-side GbE: a plain RTL8211F RGMII PHY (address 2) on BANK34 → GEM1 over EMIO + GMII-to-RGMII IP gives Linux `eth1` (PS-CONFIG.md §5). No PL MAC needed.
- [ ] Real DSP48 utilisation for the audio chain — settle by synthesis, replacing my 📐 estimate.
- [ ] Achievable PL clock for the audio pipeline (affects the MAC/s headroom above).

## Sources
- Puzhi PZ7020-StarLite specs — https://www.en.puzhi.com/detail/374.html
- Vivado edition/device support (WebPACK covers XC7Z020) — https://adaptivesupport.amd.com/s/article/42072
- Vivado editions & install footprint — https://pcbsync.com/xilinx-vivado-editions/
- Zynq PS-PL AXI GP/HP/ACP overview — https://www.aldec.com/en/company/blog/145--demystifying-axi-interconnection-for-zynq-soc-fpga
- XC7Z020 HP/ACP measured throughput (>1.6 GB/s @125 MHz) — https://www.researchgate.net/publication/318988860_Comparison_of_Accelerator_Coherency_Port_ACP_and_High_Performance_Port_HP_for_Data_Transfer_in_DDR_Memory_Using_Xilinx_ZYNQ_SoC
