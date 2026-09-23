# Off-grid in-car system — index

BMW F30 330e (PHEV). Voice-controlled offline navigation, CAN logging, sentry
cameras, media, and an off-grid network — built from parts on hand.

---

## The chain, end to end

```
BMW bus ──(parallel tap, listen-only)──> Teensy logger ──USB "r" stream──┐
                                                                        │
ESP32-S3 climate node ──serial──────────────────────────────────────────┤
GPS ────────────────────────────────────────────────────────────────────┤
                                                                        ▼
                                                              can_bridge + hub
                                                              (Orange Pi 4 Pro)
                                                                        │
                        ┌───────────────────────────────┬───────────────┤
                        ▼                               ▼               ▼
                  vent display                   voice assistant    HTTP /api
                  (Teensy + 2.4")                (whisper→Valhalla)  dashboard

Luckfox pods ──H.264 (hardware encode)──> storage        Zynq ──> audio front-end
(sentry, low power)                                      (beamform + AEC)
```

---

## Firmware

| Dir | Runs on | Does |
|---|---|---|
| [`firmware/car-can-logger`](firmware/car-can-logger) | Teensy 4.1 | **Hardware listen-only** CAN capture → SD; live `r` raw stream; ID census with changed-bit masks |
| [`firmware/vent-display`](firmware/vent-display) | Teensy 4.1 | 2.4" dashboard in the vent; 3 pages; only redraws changed fields |
| [`firmware/vent-climate-node`](firmware/vent-climate-node) | ESP32-S3 | Damper servo + temp. **Cold air → OPEN (free cooling), hot → CLOSED.** Fail-closed |

## Software

| Dir | Runs on | Does |
|---|---|---|
| [`firmware/telemetry-hub`](firmware/telemetry-hub) | Orange Pi | `hub.py` — value store with **age/staleness**, HTTP API, feeds the display. `can_bridge.py` — frames → named signals via `signals.json` |
| [`firmware/voice-assistant`](firmware/voice-assistant) | Orange Pi | "hey car, take me to the closest Petco" — whisper → grammar → SQLite R-tree POI → Valhalla → Piper |
| [`firmware/sentry-camera`](firmware/sentry-camera) | Luckfox | Motion-triggered recording with **pre-roll**, power gate, hardware H.264 |

## Hardware & models

| Path | |
|---|---|
| [`hardware/3d`](hardware/3d) | vent insert (+airflow, carrier bosses), Teensy carrier, compute box, power box, 4-mic bar, knob, servo bracket, camera pod |
| [`hardware/bmw-f30-bus-reference.md`](hardware/bmw-f30-bus-reference.md) | BMW buses, the **60 Ω verification test**, why no 74HC splitting |
| [`hardware/interconnect.md`](hardware/interconnect.md) | how every node talks |
| [`hardware/orange-pi-4-pro-reference.md`](hardware/orange-pi-4-pro-reference.md) | day-one Pi reference |
| [`hardware/pz7020-starlite/`](hardware/pz7020-starlite) | FPGA capability, boot runbook, pinout sheet |
| [`hardware/voice-nav-pipeline.md`](hardware/voice-nav-pipeline.md) | the "hey car" pipeline |
| [`hardware/car-system-architecture.md`](hardware/car-system-architecture.md) | roles, power, storage, failure modes |

---

## Verify everything

```bash
# firmware
(cd firmware/car-can-logger   && arduino-cli compile --fqbn teensy:avr:teensy41 .)
(cd firmware/vent-display     && arduino-cli compile --fqbn teensy:avr:teensy41 .)
(cd firmware/vent-climate-node&& arduino-cli compile --fqbn esp32:esp32:esp32s3 .)

# software (no deps, no data)
python firmware/voice-assistant/intent.py --selftest
python firmware/voice-assistant/build_poi_index.py --selftest
python firmware/voice-assistant/assistant.py --selftest
python firmware/telemetry-hub/hub.py --selftest
python firmware/telemetry-hub/can_bridge.py --selftest
python firmware/sentry-camera/sentry.py --selftest

# models: every one must report Volumes: 2 (one closed solid)
openscad -D 'render_mode="print"' -o /tmp/x.stl hardware/3d/vent-insert.scad
```

---

## Rules this build follows

1. **The car side is silicon-silent.** FlexCAN `LISTEN_ONLY` means the controller
   physically cannot transmit, ACK or error-frame. Not a software policy.
2. **Remove the 120 Ω resistor** from any transceiver breakout before tapping — the
   car already has exactly two.
3. **Sleep with the car.** A node that keeps a bus awake kills the 12 V battery and
   throws unrelated faults.
4. **Stale data is withheld, never shown as live.** Every value carries an age.
5. **No LLM in the driving path.** The intent parser is a grammar: instant,
   deterministic, and it fails loudly instead of inventing a destination.
6. **Buffer locally, sync opportunistically.** Never assume a link is up.
7. **Print in ASA or PETG, never PLA** — a dash in sun exceeds PLA's ~60 °C Tg.
8. **Check `Volumes: 2`** before slicing any model. Two fragmentation bugs were
   caught this way.

---

## Blocked, and on what

| Blocked | Needs |
|---|---|
| Printing the vent parts | **5 caliper measurements** — see [`hardware/3d/README.md`](hardware/3d/README.md) |
| Printing the enclosures | Board **mounting-hole patterns** (not published for either board) |
| All FPGA gateware | The **AITH Dropbox bundle** (`.xdc` + boot image) |
| Live signals on the display | Decode them: `canlog.py candidates` → add to `signals.json` |
| Voice assistant on real audio | GPS fix, mic array → Zynq, OSM extract + Valhalla tiles |

`signals.json` ships **empty on purpose** — inventing plausible BMW IDs would put
confident wrong numbers on a dashboard you read while driving.
