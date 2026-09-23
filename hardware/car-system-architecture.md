# Off-grid in-car system — architecture

An in-car monitoring / logging / media / off-grid-WiFi system built from parts on hand.
Design principle: **network-centric** — a Gigabit switch + WiFi AP is the spine, every
smart node is an IP address, and that spine *is* the off-grid network. Match each chip to
what it's actually good at; don't cross the wires.

## Nodes and roles

| Node | Role | Why this chip |
|------|------|---------------|
| **Orange Pi 4 Pro** (Allwinner A733, 4 GB LPDDR5, GPU + 3 TOPS NPU, WiFi6, HDMI 2.0, NVMe PCIe3, USB3, GbE) | **Head unit**: media playback → HDMI screen; dashboard/UI (web); AI inference (NPU); media + file server; logging database; **WiFi6 AP + LAN router**; owns the bulk storage | HW 4K decode, NPU, WiFi6, fast NVMe+USB3 buses, strong A76 cores |
| **Zynq XC7Z020** (PZ7020-StarLite: dual A9 PS + Artix-7 PL, 220 DSP48, 1 GB DDR3, 2× GbE) | **Real-time acquisition + DSP co-processor**: aggregate/timestamp CAN & sensors, audio/camera/SDR DSP, deterministic control; stream results to the Pi | 220 DSP48, jitter-free fabric, 2× GbE, no OS in the timing loop |
| **Teensy ×N** (4.x, FlexCAN) | **CAN edge** on the vehicle buses; µs-precise capture/inject | FlexCAN / CAN-FD, deterministic |
| **ESP32-S3** | **BLE/IoT bridge + control/guest SSID + low-power wake node** (NOT the main AP) | WiFi/BLE combo, low power, always-on |
| **~4.5 TB storage** | Media library + logs, on the Pi (NVMe = log DB for write endurance; 2 TB USB-C SSD = media); shared over LAN (SMB/NFS) | Pi has the fast buses; Zynq SD/USB2 is too slow to host storage |

## Interconnect

- **FPGA ↔ Orange Pi: Gigabit Ethernet** (primary data), **SPI/UART** (control plane).
  Both boards have GbE; sockets are simple, high-bandwidth, decoupled, and this doubles as
  the LAN. **PCIe** (Pi M.2 ↔ Zynq PL endpoint) is faster but heavy to build and competes
  with the NVMe slot — not worth it here.
- **Teensy ↔ Zynq**: UART/SPI (or CAN) — Teensy does the µs-critical bus work, Zynq
  aggregates. Keep timed peripherals **off** the ESP32 (WiFi jitter).
- **Spine**: one small **GbE switch + WiFi6 AP** (the Pi can be the AP, or a 12 V travel
  router). Pi + Zynq + ESP32 + head screen all hang off it.

```
  vehicle CAN ── Teensy(s) ──UART/SPI──► Zynq 7020 ──GbE──►┐
  cameras/RF ───────────────MIPI/PL───► (real-time DSP)    │
                                                           ├─► Orange Pi 4 Pro ──HDMI──► car screen
  ESP32-S3 (BLE/IoT + guest SSID) ──────GbE/WiFi───────────┤     • media playback (HW decode)
                                                           │     • NPU inference
  passenger phones/tablets ──WiFi6──────────────────────── ┘     • logging DB  +  media server
                                                                 • WiFi6 AP / LAN router
                                                                 • 4.5 TB storage (NVMe + 2 TB SSD)
```

## Use-case data flows
- **Monitoring / logging:** CAN → Teensy → Zynq (timestamp/aggregate/DSP) → GbE → Pi (log to DB on SSD, live dashboard, alerts).
- **Shows / media:** 4.5 TB library on Pi → HW decode → HDMI screen, **and** streamed over WiFi to devices (Jellyfin/Plex-style server).
- **Off-grid WiFi:** Pi WiFi6 AP = the network; devices connect → stream media, view dashboard, pull logs.
- **AI / vision (optional):** camera → Zynq (MIPI pre-process) → Pi NPU (detection) → overlay/alert.
- **SDR / RF (optional):** antenna → Zynq PL (FFT/DDC) → Pi (classify/log) — the RTL-SDR path drops in here.

## Design calls (measured truth)
1. **ESP32-S3 is not the main hotspot.** Real-world AP throughput (a few Mbps, few clients) can't serve HD video to multiple devices. Use the **Pi's WiFi6** (or a 12 V travel router) for media; give the ESP32 the **BLE sensors + a control/IoT SSID** where it's genuinely better.
2. **No video/media on the Zynq.** It has no usable video engine — that's the Pi's GPU/VPU. Zynq stays real-time/DSP only.
3. **Power is the #1 reliability risk.** 12 V rails are noisy and sag on cranking. Use a **12 V→5 V DC-DC with hold-up / UPS** (Pi ~5 V/4 A, Zynq 5 V/1 A, plus SSD + fans on 12 V). A crank brownout mid-write corrupts the SD/SSD. This is also why active cooling (the fan) matters — a car cabin runs hot.
4. **Storage split:** log DB on the SSD/NVMe (endurance), media on the largest volume; share over LAN (SMB/NFS) so the Zynq can write logs straight to the Pi's store.

---

# Deep design — the things that actually make or break this

## 1. The CAN safety boundary (do this or nothing else matters)
This system touches a **vehicle CAN bus** and also hosts **passenger WiFi**. If any path
exists from a guest device to a node that can *write* CAN, that is a genuine safety hazard,
not a security inconvenience. Enforce the boundary in **hardware**, not policy:

- **Teensy CAN nodes run in listen-only / silent mode.** FlexCAN supports a hardware
  listen-only mode where the controller never asserts a dominant bit — it physically cannot
  transmit, ACK, or error-frame the bus. This is enforced by silicon, not by your code.
- **One-way link (data diode):** wire only **Teensy TX → Zynq RX**. Leave the reverse wire
  off. No command can travel toward the vehicle because there is no conductor for it.
- **Network segmentation:** guest/media SSID on its own VLAN/subnet with **no route** to the
  acquisition segment. The Pi routes media; it does not bridge to the CAN side.
- Only break read-only if you deliberately, knowingly add a write path — and then put a
  physical switch in it.

## 1b. Decision: video playback stays on the Pi's CPU — NOT on the FPGA
**Considered and rejected:** using the Zynq as a "GPU" to play media off an SD card.

Rejected for three checkable reasons:
- **Zynq-7000 has no hardware video codec block.** (The VCU is in Zynq *UltraScale+ EV*, a
  different part.) Any decode would be custom PL logic or PS software.
- **Memory makes a fabric decoder infeasible:** one 720p YUV420 frame is
  1280×720×1.5 = **1.32 MB**, but the XC7Z020 has only **~613 KB total BRAM** (4.9 Mb). A single
  frame exceeds all on-chip RAM, and H.264 needs several reference frames — so everything would
  run out of DDR3 behind a custom pipeline. Full H.264 decoders are commercial, multi-year IP.
- **The Zynq's CPUs are weaker:** 2× Cortex-A9 @ 766 MHz vs the Pi's **2× Cortex-A76 @ 2.0 GHz**.
  Moving playback there makes it slower.

**Decision: decode in software on the Pi. Resolution ceiling is UNKNOWN and must be measured —
do not assume a limit.**

⚠️ **Correction on record:** an earlier version of this doc claimed "1080p is marginal, 4K is
out." **That was asserted without evidence and is probably wrong.** Evidence found afterwards
points the other way: the **Raspberry Pi 5 shipped with its H.264 hardware decoder removed** and
plays 1080p H.264 in software on Cortex-A76 cores — the same core family as the A733's. Our Pi
has fewer/slower A76s (2× @ 2.0 GHz vs Pi 5's 4× @ 2.4 GHz), so the ceiling is lower, but
**nobody has measured where.**

Measure it instead of guessing — start at 1080p, not 720p:
```bash
ffmpeg -i clip.mp4 -f null - 2>&1 | tail -3     # speed= above 1.0x == real time
```
Work *down* from 1080p to find the true ceiling. Do not design around an invented limit.

The FPGA stays on the job nothing else can do: the **audio front-end** (mic-array beamforming,
echo cancellation, noise suppression) and real-time capture/timestamping.

## 2. Time synchronization — the non-obvious make-or-break for logging
Multi-node logs are worthless if timestamps don't correlate. Linux gives you milliseconds
and jitter; the FPGA gives you sub-microsecond.

- **Make the Zynq the time master.** Hardware-timestamp every CAN frame / sample in the PL
  at ns resolution as it arrives. Those stamps are authoritative.
- The Pi records only *arrival* time; it never re-stamps. This sidesteps clock-sync entirely
  for correctness — the Zynq's counter is the single timeline.
- If you need absolute (wall-clock) time: feed a **GPS PPS** into the PL and discipline the
  counter to it. That gives ns-accurate absolute timestamps — the professional approach, and
  it makes logs correlatable with anything external.
- If you'd rather sync clocks: **PTP (IEEE 1588)** over the GbE link beats NTP by ~1000×.

## 3. Storage: endurance is fine — **power-loss atomicity is the real threat**
Run the numbers rather than guessing. A busy 500 kbit/s CAN bus at ~50% utilization is
~31 KB/s of frames; with timestamps/metadata call it ~100 KB/s. Continuous that's ~8.6 GB/day
≈ 3.1 TB/year. Against a 2 TB consumer SSD's typical 600+ TBW rating, that's *decades*.
**Wear is a non-issue at CAN rates.**

What will actually bite you:
- **Write amplification** from a database doing tiny synchronous writes, plus filesystem
  journaling, can multiply writes 10–100×. Fix: **buffer and write in large sequential
  chunks**, use an **append-only log** format, and never `fsync()` per frame.
- **Corruption on power loss** mid-write (crank, ignition-off). Fix: read-only root (below)
  + append-only data + a hold-up rail that guarantees a clean flush.
- **Read-only root filesystem with an overlay.** The OS partition never changes, so it
  cannot be corrupted by a sudden power cut. All mutable data lives on a separate data
  partition. This is standard automotive/embedded practice and eliminates most failures.

## 4. Power: the #1 killer of car builds
- **Cranking** drags 12 V down to ~6 V (sometimes ~4.5 V) for tens of milliseconds. A plain
  buck converter browns out and your filesystem pays for it.
- Budget: Pi ~5 V/4 A peak, Zynq 5 V/1 A, SSD ~5 V/1 A, plus fans → plan for ~30 W+.
- Use a **wide-input automotive DC-DC (9–36 V) with crank ride-through**, plus a
  **supercapacitor bank or small LiFePO4 UPS** sized to hold the rail long enough for a
  clean shutdown.
- **Ignition-sense input** → the system learns "power is leaving" → flush, sync, halt. This
  is not optional; it's what converts a power cut into a graceful shutdown.
- **Standby drain:** a Pi + Zynq idling will flatten a car battery in a day or two. Put a
  **low-power wake node** (Teensy or ESP32-S3) in charge of sleeping/waking the big boards —
  e.g. **wake on CAN activity** or ignition. Architecturally required, not a nicety.

## 5. What specifically belongs in the PL (the FPGA's real, unique job)
The honest economic argument for the FPGA is **line-rate data reduction**: it eats a
firehose and hands the Pi only what matters.
- **Hardware CAN timestamping** (ns) and multi-bus merge into one ordered stream.
- **Deterministic multi-channel sampling** — simultaneous ADC capture at exact intervals
  (the Pi fundamentally cannot; the OS scheduler ruins it).
- **Real-time audio DSP** — EQ/crossover/active noise reduction at fixed, known latency.
- **Camera pre-processing** — raw MIPI → dewarp / HDR / motion-gate *before* the Pi's NPU,
  so the NPU only runs on frames that matter (huge CPU saving).
- **SDR front-end** — FFT/DDC/filtering on the 220 DSP48 slices.

## 6. Bandwidth budget — where the real bottleneck is
- CAN: <1 Mbit/s total across several buses. Trivial.
- Zynq↔Pi GbE: ~940 Mbit/s practical — oversized for CAN, correctly sized for camera/SDR.
- Media from storage: NVMe PCIe3 = GB/s; USB3 SSD ~400 MB/s. Both far exceed 4K playback.
- Pi WiFi6 AP: realistically ~200–400 Mbit/s shared — several HD streams, fine.

**Conclusion: nothing in the data path is a bottleneck. The only real limits are Pi RAM
(4 GB) and Pi CPU *if you transcode*.** Hence the rule: **direct-play media, never
transcode.**

## 7. Failure modes — degrade, don't die
Design rule: **every node buffers locally and syncs opportunistically.** Never assume the
link is up.
- Zynq dies → Pi still serves media + WiFi; logging stops (and says so).
- Pi dies → Zynq keeps logging to its own SD, syncs when the Pi returns.
- Network dies → both buffer locally, reconcile later.
- Each node exposes a heartbeat so the dashboard shows what's actually alive.

## 8. Memory reality (Orange Pi 4 Pro)
RAM is **soldered LPDDR5 — not upgradeable**. No DIMM, no HAT: a DDR bus is ~200+ GHz-speed
length-matched signals and cannot traverse a low-speed pin header. Expansion on this board is
storage only (eMMC, SPI flash, M.2 NVMe, microSD). Note the 4 GB variant clocks at
**2400 MHz** vs **2040 MHz** on the 6/8/12 GB parts — less capacity, faster memory.
Mitigations: **zram** (≈2–3× on compressible data), **NVMe swap**, offload buffering to the
Zynq's own 1 GB DDR3, and **never transcode**.

## Open items (gating)
- Zynq boot image + `.xdc` — from the AITH Dropbox bundle (see [pz7020-starlite/BOOT-SD-runbook.md](pz7020-starlite/BOOT-SD-runbook.md)).
- Fan control pin — needs the `.xdc` (see [pz7020-starlite/README.md](pz7020-starlite/README.md)).
- Confirm total storage devices summing to ~4.5 TB and their buses (NVMe vs USB).
