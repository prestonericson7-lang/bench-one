# "Hey car, take me to the closest Petco" — the offline pipeline

Fully offline. Every stage mapped to the silicon that's actually good at it, with the
constraints that come from *this* hardware rather than a generic write-up.

**Confidence:** ✅ sourced · 📐 calculation · ⚠️ unverified.

---

## The chain

| # | Stage | Runs on | Stack |
|---|-------|---------|-------|
| 1 | **Wake word** ("hey car") — always on, low power | **ESP32-S3** | ESP-SR / WakeNet — the S3 has instructions specifically for this |
| 2 | **Clean the audio** | **Zynq FPGA** | mic-array beamforming + AEC + noise suppression |
| 3 | **Speech → text** | **Orange Pi** | `whisper.cpp`, **`tiny.en`** |
| 4 | **Intent + POI lookup** | **Orange Pi** | grammar/intent match + **SQLite R-tree** over OSM POIs |
| 5 | **Route** | **Orange Pi** | **Valhalla** on a regional OSM extract |
| 6 | **Speak** | **Orange Pi** | **Piper** TTS |

---

## Stage 2 — why the FPGA earns its place here
A moving car is the worst realistic case for ASR: road roar, wind, HVAC, and **your own music
playing through the speakers you're listening over**. Fixed-latency multichannel DSP is exactly
what fabric is for, and it's work the Pi has no spare CPU for.

📐 **Resource calculation** (assumptions stated): AEC as 512-tap NLMS at 16 kHz voice rate =
(512 filter + 512 update) × 16,000 = **~16.4 M MAC/s**. One DSP48 at 100 MHz ≈ **100 M MAC/s**.
→ **~0.2 of one slice, out of 220.** A 4-mic adaptive beamformer is the same order. The whole
front-end plausibly fits in **under 10 of 220 slices**. *Validate in simulation before building.*

**This stage is what makes stage 3 viable** — see below.

---

## Stage 3 — the real constraint on this board ✅

Measured on a **Raspberry Pi 5 (4× Cortex-A76 @ 2.4 GHz)**:
- `tiny` (39 M params, ~75 MB) — **real time**
- `base` (74 M params, ~142 MB) — **~real time, but "with `-t 4`"** (four A76 threads)
- `small` — **below real time**, batch only
- No GPU path exists; it's all CPU/ARM NEON. Quantized GGML (`ggml-*-q5_0.bin`) cuts memory
  and speeds inference.

> ⚠️ **Our board has only 2× A76** @ 2.0 GHz (plus 6× A55 @ 1.8 GHz). Since `base` needs four
> A76 threads to hit real time on a faster Pi 5, **`base` is marginal here and `tiny.en` is the
> realistic choice.** The A55s can be thrown in (`-t 8`) but contribute far less per core.

**This is precisely why stage 2 matters:** a smaller model is more sensitive to noisy input, so
cleaning the audio in fabric is what makes `tiny.en` accurate enough to trust at highway speed.

Precedent for the whole shape: **whisper.cpp + Piper TTS running together on ARM64 (RK3588)**.

---

## Stage 4 — do NOT put an LLM in the driving path
For a bounded command vocabulary, a **grammar/intent match plus fuzzy POI lookup** beats a
language model on every axis that matters in a moving car: it's deterministic, instant, uses
almost no RAM, and cannot hallucinate a destination.

**POI search:** build a **SQLite database with an R-tree spatial index** over the OSM extract
(name, brand, category, lat/lon). "Petco" → match name/brand → nearest by distance from the GPS
fix → hand coordinates to Valhalla. Tiny RAM, sub-millisecond, and SQLite is stdlib-grade
dependable.

A small LLM can be added later for free-form phrasing — but as a *fallback*, never as the thing
standing between you and the route.

---

## Stage 5 — Valhalla, and the reason is RAM ✅

| Engine | Planet-scale requirement | Verdict for us |
|---|---|---|
| **Valhalla** | **tile-based**, ~100 GB disk, **moderate RAM — loads only the tiles you query** | ✅ **chosen** |
| OSRM | **~55 GB RAM** (mmap/shared memory helps, MLD algorithm) | ❌ RAM |
| GraphHopper | **40–60 GB JVM heap** | ❌ RAM |

Sources explicitly recommend Valhalla when you "have a low-spec device," because the tile model
means memory scales with the area queried, not the dataset.

**And we don't need the planet.** A state/region extract is a few GB — trivial against **4.5 TB**
of storage. Valhalla's disk-heavy tradeoff costs us nothing and buys low RAM, which is our
actual scarce resource.

Put the tiles on the **NVMe** so tile faults are fast.

---

## Stage 6 — Piper TTS
Small models, good quality, runs on ARM; proven alongside whisper.cpp on ARM64. Low RAM.

---

## Mic array — spec'd and orderable

**Parts:** **SPH0645** or **INMP441** I²S MEMS mic modules — both 24-bit, mono, with an **L/R
channel select** pin ✅. **Prefer the SPH0645: 65 dB SNR vs the INMP441's 61 dB**, and a lower
noise floor. In a car, 4 dB of SNR at the microphone is worth more than any amount of downstream
processing.

**Wiring trick:** because each module has L/R select, **two mics share one I²S bus** (one on
left, one on right). So 4 mics = **2 I²S buses** into the Zynq PL. Clock them from a **single
shared master clock** — phase coherence between mics is the whole basis of beamforming, so they
must sample on the same clock edge.

### 📐 Mic spacing — derived, not guessed
Spatial Nyquist: to avoid grating lobes, spacing `d ≤ λ/2` at the highest frequency of interest.
With c = 343 m/s and Whisper's **16 kHz sample rate → 8 kHz Nyquist**:

> λ(8 kHz) = 343 / 8000 = **42.9 mm** → **d ≤ ~21 mm between mic centres**

(If the band is limited to 4 kHz telephony, d ≤ ~43 mm.) **Target ~20 mm spacing**, uniform, and
**record the exact spacing** — the beamformer's delays are computed from it, so an error here
directly steers the beam the wrong way.

**Mount them away from the blower outlet.** The AC vent is a noise source pointed at the cabin;
putting mics in its airflow adds wind noise the AEC cannot remove (it's uncorrelated with the
speaker reference signal).

## What this needs that we don't have yet
- [ ] **GPS** for "closest" — a module on the Teensy or Pi (you said you can add one).
- [ ] **4× SPH0645** (or INMP441) + wiring to the Zynq over **I²S direct**, ~20 mm spacing,
      shared master clock — not over the network.
- [ ] A **reference audio tap** of what the speakers are playing, for the AEC to subtract.
- [ ] Regional **OSM extract** + built Valhalla tiles + the SQLite POI index.

## Memory budget sanity check (4 GB)
whisper `tiny.en` ~75 MB + Piper ~60 MB + Valhalla (mmap'd tiles, scales with query area) +
SQLite (negligible) — comfortably inside 4 GB **provided** we hold the rules from
[car-system-architecture.md](car-system-architecture.md): **zram on, never transcode media, and
no LLM resident in RAM.**

## Sources
- whisper.cpp tiny/base real-time on Pi 5 A76 — https://gotranscript.com/public/enhance-raspberry-pi-5-with-whisper-for-live-transcription
- whisper.cpp + Piper TTS on ARM64 (RK3588) — https://turingpi.com/whisper-cpp-piper-tts-arm64-turing-pi-rk3588/
- Routing engine RAM comparison / Valhalla for low-spec devices — https://www.pistack.xyz/posts/2026-04-25-graphhopper-vs-osrm-vs-valhalla-self-hosted-routing-engines-guide-2026/
- Routing engine overview — https://gis-ops.com/open-source-routing-engines-and-algorithms-an-overview/
- SPH0645 vs INMP441 (SNR 65 dB vs 61 dB, I²S, L/R select) — https://zbotic.in/mems-microphone-sph0645-high-quality-i2s-audio-capture-with-arduino-raspberry-pi/
- INMP441 datasheet — https://www.farnell.com/datasheets/1824785.pdf
