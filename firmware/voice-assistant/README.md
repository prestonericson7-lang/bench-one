# voice-assistant — "hey car, take me to the closest Petco"

Fully offline. No cloud, no account, no signal required.

```
wake word (ESP32-S3)  ->  clean audio (Zynq FPGA)  ->  whisper.cpp tiny.en
   -> intent.py (grammar)  ->  poi.db (SQLite R-tree)  ->  Valhalla  ->  Piper
```

| File | Does |
|---|---|
| `intent.py` | sentence → structured command (**grammar, not an LLM**) |
| `build_poi_index.py` | OSM extract → SQLite **R-tree** POI index, + nearest-search |
| `assistant.py` | wires the chain together; degrades gracefully when tools are missing |

Everything is **stdlib-only** and every file **self-tests with no data and no
dependencies installed**:
```bash
python intent.py --selftest            # 12/12
python build_poi_index.py --selftest
python assistant.py --selftest
```

---

## Why a grammar and not an LLM

A driving command sits between you and a moving car. A small model on 4 GB would be slower,
hungrier, non-deterministic, and able to **hallucinate a destination**. The grammar is instant,
uses no RAM, and **fails loudly** (`action=unknown` → "Sorry, I didn't catch a destination")
rather than confidently inventing one.

An LLM can be bolted on later as a **fallback** for free-form phrasing — never as the thing
standing between the driver and the route.

It also handles the reality of speech-to-text: whisper mangles brand names, so there's a
phonetic-correction table (`pet co → petco`, `wal mart → walmart`, …).

> **Bug caught by the selftest:** that table used naive substring replacement, so the entry
> `"home depo" → "home depot"` fired on the *already correct* "home depot" and produced
> **"home depott"**. Now matched on word boundaries.

---

## Why Valhalla for routing

| Engine | Planet-scale needs | Verdict |
|---|---|---|
| **Valhalla** | tile-based, ~100 GB disk, **loads only the tiles queried** | ✅ chosen |
| OSRM | **~55 GB RAM** | ❌ |
| GraphHopper | **40–60 GB JVM heap** | ❌ |

**RAM is our scarce resource (4 GB); disk is not (4.5 TB).** Valhalla trades exactly the way we
need. And we don't need the planet — a regional extract is a few GB. Put the tiles on the NVMe.

## Why SQLite + R-tree for POI search

Valhalla routes between coordinates; **it does not search for places.** So "closest Petco" needs
a separate index. SQLite ships with Python, the R-tree module turns "nearest within a box" into
an indexed query instead of a scan over every POI in the state, and RAM cost is ~nothing.

> **Gap caught by the selftest:** *"take me to the nearest gas station"* found nothing even with
> a fuel station 300 m away — the search only matched **name/brand**, and a generic category
> request has no brand. Category search is now wired, with a name-match fallback.

---

## Why `tiny.en` and not `base`

Measured on a **Raspberry Pi 5 (4× Cortex-A76 @ 2.4 GHz)**: `tiny` and `base` both run in real
time, but **`base` needs `-t 4`** — four A76 threads. **Our board has only 2× A76** @ 2.0 GHz
(plus 6 weaker A55s), so **`base` is marginal and `tiny.en` is the realistic choice.**

This is exactly why the **FPGA audio front-end earns its place**: a smaller model is more
sensitive to noisy input, and a moving car is the worst case (road, wind, HVAC, and your own
music through the speakers you're listening over). Cleaning the audio in fabric is what makes
`tiny.en` trustworthy at highway speed. See
[../../hardware/voice-nav-pipeline.md](../../hardware/voice-nav-pipeline.md).

---

## Setup on the Pi

```bash
# 1. whisper.cpp (CPU/NEON; there is no GPU path on this SoC)
git clone https://github.com/ggml-org/whisper.cpp && cd whisper.cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
bash ./models/download-ggml-model.sh tiny.en          # ~75 MB
# quantised models (ggml-*-q5_0.bin) cut RAM and speed inference further

# 2. Piper TTS
#    grab a release binary + an en_US voice .onnx

# 3. Valhalla + a regional OSM extract (Geofabrik), tiles on the NVMe
#    then run valhalla_service on :8002

# 4. POI index from the same extract
python build_poi_index.py --osm region.osm --out poi.db
```

Then:
```bash
python assistant.py --say "take me to the closest Petco" --lat 40.01 --lon -75.00
python assistant.py --wav clip.wav --lat 40.01 --lon -75.00
```
Overridable via env: `WHISPER_BIN WHISPER_MODEL PIPER_BIN PIPER_VOICE VALHALLA_URL POI_DB`.

---

## Design rule: nothing may take down navigation
Every external tool is probed at runtime. Missing whisper, Piper or Valhalla produces a spoken
or printed message, **never a traceback**. A dead text-to-speech must not kill the route.

## Still needed
- [ ] **GPS** for the current fix (`--lat/--lon` are placeholders for it).
- [ ] Wake-word trigger from the ESP32-S3 → this process.
- [ ] Mic array → Zynq → clean audio → `.wav` handed to `transcribe()`.
- [ ] Regional OSM extract + built Valhalla tiles + `poi.db`.
