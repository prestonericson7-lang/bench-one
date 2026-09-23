# Orange Pi 4 Pro (Allwinner A733) — readiness reference

Everything needed to use this board the day it arrives. Sources cross-checked; nothing
here rests on a single hit.

**Confidence marks**
- ✅ **Confirmed** — two or more independent sources agree.
- ⚠️ **Single source** — plausible but unverified; treat as provisional.
- ❌ **Contradicted / wrong** — recorded so nobody repeats the error.

---

## 1. SoC — Allwinner A733

| Item | Value | Conf |
|------|-------|------|
| CPU | **2× Cortex-A76 @ 2.0 GHz + 6× Cortex-A55 @ 1.8 GHz** | ✅ (ArmSoM + CNX agree) |
| Extra core | RISC-V **E902 @ ~200 MHz** coprocessor | ✅ |
| Process | 12 nm | ⚠️ |
| NPU | **VeriSilicon/Vivante VIP9000, 3 TOPS INT8** (INT4/INT8/INT16/FP16/BF16) | ✅ |
| GPU | Imagination **BXM-4-64 MC1** | ✅ (official datasheet) |
| **VPU decode** | **H.265 / VP9 / AVS2 up to 8K @ 24 fps** | ✅ (official datasheet) |
| **VPU encode** | **H.265 / H.264 up to 4K @ 30 fps** | ✅ (official datasheet) |
| Memory | LPDDR4 / LPDDR4x / **LPDDR5**, up to 16 GB | ✅ |
| PCIe | **PCIe 3.0 ×2** | ✅ |
| USB (SoC) | **USB 3.1 Gen2 DRD** | ✅ |
| Display out (SoC) | HDMI, RGB, LVDS, MIPI-DSI, **eDP 1.4b / DP 1.4**, E-ink | ✅ |
| Camera in (SoC) | MIPI CSI + Parallel CSI | ✅ |
| Operating temp | **−25 °C to 115 °C** | ⚠️ |
| Power | **~4 W idle, ~8 W peak** | ⚠️ |

> ❌ **Correction on record:** Armbian PR #10712's description claims *"4× A76 + 4× A55."*
> That is **wrong**. It is **2× A76 + 6× A55**. Plan compute around **two** fast cores.

### Our board
4 GB LPDDR5. Note the 4 GB variant clocks its memory at **2400 MHz** vs **2040 MHz** on the
6/8/12 GB parts — less capacity, **faster** memory. RAM is **soldered; not upgradeable**
(no DIMM, no HAT — a DDR bus can't cross a GPIO header). See
[car-system-architecture.md](car-system-architecture.md) §8 for mitigations (zram, NVMe swap).

---

## 2. Board hardware

| Interface | Detail | Conf |
|---|---|---|
| Storage | microSD, optional eMMC socket, **16 MB SPI-NOR (Winbond W25Q128JW)**, **M.2 M-Key NVMe (PCIe 3.0)** | ✅ |
| Ethernet | Gigabit, **Motorcomm YT8531** PHY — measured **>940 Mbit/s** | ✅ |
| Wireless | **WiFi 6 + BT 5.4**, chip is **AIC8800DC** over SDIO | ✅ |
| USB | 1× USB 3.0 host, 3× USB 2.0 host, 1× USB 2.0 OTG Type-C | ⚠️ |
| Video out | HDMI (marketed 2.0/4K60); **Linux today = 1080p60 framebuffer** | ✅ |
| Camera/display | MIPI CSI 2-lane + 4-lane, MIPI DSI 4-lane | ⚠️ |
| GPIO | 40-pin, Raspberry-Pi-compatible | ✅ |
| PMIC | **AXP8191** over I²C | ⚠️ |
| Other | 12-bit SAR ADC (GPADC), 16-ch PWM, 5 thermal zones, crypto engine CE V5 + `/dev/hwrng` | ⚠️ |
| Size | 89 × 56 mm | ⚠️ — **measure on arrival before modelling an enclosure** |

---

## 3. Software reality — read this before planning anything

The board is new (late 2025) and **support is immature**. Two sources appeared to conflict;
they describe **different points in time**, and both are true:

### Today — official released image
- **Orange Pi Ubuntu Jammy, Linux 6.6.98-sun60iw2** — boots and works.
- ❌ **NVMe/PCIe:** users report *"no driver to install nvme."*
- ❌ **GPU:** *"notably missing is support for the GPU"* — no acceleration.
- ❌ **NPU:** *"doesn't seem documented yet."*
- ⚠️ **WiFi:** partial; needs out-of-tree driver (`lynxlikenation/aic8800`).
- ✅ Ethernet, SD, eMMC fine.
- **Effectively: a fast headless Linux server.**

### Coming — Armbian PR #10712 (kernel 7.2 edge, validated on 7.2.6)
Claims working: NVMe **>450 MB/s**, Ethernet **>940 Mbit/s**, AIC8800 WiFi, Bluetooth,
USB 3.0 + USB 2.0, SPI-NOR, SD/eMMC, **VIP9000 NPU**, crypto+TRNG, ADC, PWM, 5-zone thermal,
AXP8191 PMIC.

**Caveats that survive the optimism:**
- **PR is OPEN, not merged** — not in a released Armbian yet.
- Review raised **30 major issues**, incl. a **cpufreq stub that changes CPU voltage without
  changing frequency → undervolting risk**.
- **Still no GPU acceleration, and no hardware video decode is claimed anywhere.**
- HDMI is **1080p60 via framebuffer**, not the marketed 4K60.

### 🟡 Architectural consequence — a driver problem, not a silicon problem
The **hardware is fully capable**: the A733 datasheet specifies a VPU doing **8K@24 decode**
and **4K@30 encode**, plus the BXM GPU. What's missing today is the **Linux driver** — current
images give a plain framebuffer with no acceleration, so video falls to the **two** A76 cores.

⚠️ **Correction on record:** an earlier draft of this doc claimed "1080p is marginal, 4K is out."
**That was an unevidenced assertion and is probably wrong.** The **Raspberry Pi 5 shipped with
its H.264 hardware decoder removed** and plays 1080p H.264 in software on Cortex-A76 cores —
same core family. Ours is 2× A76 @ 2.0 GHz vs the Pi 5's 4× @ 2.4 GHz, so the ceiling is lower,
but **it has not been measured.** Measure it (start at 1080p and work down):
`ffmpeg -i clip.mp4 -f null -` → `speed=` above 1.0x is real time.

So media playback is **likely workable now, and better later**. Options, best first:
1. **Track the driver work.** There is an active Armbian thread *"A733 zero copy hardware
   decoding"*, and Bootlin has a long history of writing open VPU drivers for Allwinner parts.
   When that lands, full media capability appears with no hardware change.
2. Until then, keep media **≤1080p, direct-play only** (never transcode) and accept CPU decode.
3. If media is needed before the driver lands, play it on a device with working decode and let
   the Pi serve the files over the LAN.

This does **not** affect the Pi's other roles (logging, routing, ASR, file/AP services), and
the **encode** path matters later for the camera/sentry work.

---

## 4. Other images and resources
- `armbian/build` **PR #9967** — community support, reported working for **headless server** use.
- `blippu/orangepi4pro` — custom kernel with Docker, Tailscale, CIFS/Samba.
- `open-astro/openastro-orangepi4pro` — Armbian-based **Debian 13 (Trixie)** minimal CLI, **WiFi-AP ready**.
- `jonas5/orangepi-4pro-armbian` — reported **non-functional**; skip.
- **Radxa Cubie A7A / A7S use the same A733** — Radxa's docs (esp. the Vivante NPU guide) apply here.
- Upstreaming activity: `lore.kernel.org/linux-sunxi/?q=a733` and `lore.kernel.org/u-boot/?q=a733`.
- ⚠️ Armbian states it **will not maintain this board without funding** (est. $5–10k).

---

## 5. NPU toolchain (it is *not* "just run ONNX")
1. **ACUITY Toolkit** (Vivante ML SDK) imports TensorFlow, TFLite, PyTorch, Caffe, DarkNet,
   **ONNX**, Keras.
2. Workflow: **import → quantize → export NBG**.
3. Deploy the `.nbg` to the board; run via **VIPLite API** or `vpm_run`.
4. **VIPLite driver** is tiny (tens of KB) and ships in board firmware.
5. An **ONNX Runtime execution provider for VIP9000 is requested but does not exist**
   (onnxruntime issue #28244) — so there's no drop-in ONNX path yet.
- Reference driver work: `petayyyy/a733_npu_driver`.
- Frigate (NVR) has an open discussion on using the A733/VIP9000 — relevant to the camera work.

---

## 6. Day-one procedure
1. **Flash** the Orange Pi official Ubuntu Jammy image (known-booting) to microSD (8 GB+)
   with Raspberry Pi Imager / balenaEtcher / `dd`. Runs straight from SD — no install step.
2. Boot, confirm **Ethernet** (the one reliably-working interface) and get a shell.
3. **Do not touch the SPI-NOR flash.** A user **bricked their board's SPI** during an install
   attempt. Leave it alone until there's a documented recovery path.
4. Check `uname -a` for the actual kernel, then test in this order: USB3 → NVMe → WiFi → HDMI.
5. Only then consider a 7.2-based build for NVMe/NPU.
6. **Measure the board** (outline + mounting holes) before any enclosure modelling.

---

## 7. Open questions
- [x] ~~GPU model~~ — **BXM-4-64 MC1, confirmed by official datasheet.**
- [x] ~~Does the A733 have a VPU?~~ — **Yes: 8K@24 decode, 4K@30 encode. The gap is the driver, not the chip.**
- [ ] **When does usable VPU/GPU acceleration land in a kernel we can run?** (watch the Armbian
      "A733 zero copy hardware decoding" thread + linux-sunxi upstreaming)
- [ ] Board dimensions + mounting-hole pattern — **measure, don't trust 89 × 56 mm**.
- [ ] USB3 / audio / Bluetooth status on the *released* image.
- [ ] Real idle vs load power at the wall, for the 12 V supply budget (datasheet says ~4 W idle / ~8 W peak for the SoC alone — the board will draw more).

## Sources
- **Allwinner A733 Datasheet V0.93 (PRIMARY SOURCE)** — https://dl.radxa.com/cubie/a7a/docs/hw/datasheet/A733_Datasheet_V0.93.pdf
- Armbian thread: *A733 zero copy hardware decoding* (VPU driver progress) — https://forum.armbian.com/topic/61323-a733-zero-copy-hardware-decoding/
- CNX-Software A733 / Orange Pi 4 Pro — https://www.cnx-software.com/2025/10/25/35-orange-pi-4-pro-an-allwinner-a733-edge-ai-sbc-with-up-to-16gb-lpddr5-wifi-6/
- ArmSoM A733 deep dive (cores, power, thermal, PCIe) — https://www.armsom.org/post/allwinner-a733-deep-dive-why-armsom-chose-this-chip-for-sige6
- Armbian community thread (real-world status) — https://forum.armbian.com/topic/55919-35-orange-pi-4-pro-%E2%80%93-an-allwinner-a733-edge-ai-sbc-with-up-to-16gb-lpddr5-wifi-6-npu/
- Armbian PR #10712 (kernel 7.2 enablement) — https://github.com/armbian/build/pull/10712
- Radxa Vivante NPU SDK guide — https://docs.radxa.com/en/cubie/a7s/app-dev/npu-dev/cubie-acuity-sdk
