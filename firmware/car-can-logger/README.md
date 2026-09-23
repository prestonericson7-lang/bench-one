# car-can-logger — passive CAN capture + signal decoder

Two halves of one reverse-engineering rig for the **2017 BMW F30 330e**:

- **`car-can-logger.ino`** — Teensy 4.1 firmware. Captures every frame, **hardware
  listen-only**, timestamped, batched to SD.
- **`canlog.py`** — host decoder. Turns the raw log into **named signals**.

BMW's PHEV frames aren't publicly documented, so the logger is also the tool that decodes them.

---

## Safety: it physically cannot talk to your car

Both controllers come up with FlexCAN's `LISTEN_ONLY` mode, which puts the peripheral in silent
mode — **it never asserts a dominant bit**, so it cannot transmit, ACK, or error-frame the bus.
That's enforced by the controller, **not by our code**, so a firmware bug can't put traffic on
the car's network.

### ⚠️ Before you connect it
1. **Remove the 120 Ω terminating resistor from your transceiver breakout.** The car already has
   exactly two. A third drops the bus below 60 Ω and overloads the transceivers.
2. **Verify the pair first** using BMW's own procedure: with the car off, measure across
   CAN_H/CAN_L — **~60 Ω = a properly terminated live bus**. 120 Ω = one end only; open = wrong
   wires. Details in [../../hardware/bmw-f30-bus-reference.md](../../hardware/bmw-f30-bus-reference.md).
3. **Tap in parallel with a short stub.** CAN is multi-drop — BMW's own docs say bus users are
   "connected in parallel." **No 74HC splitting, no extra hardware, any number of taps.**

### Sleeping with the car
F-series buses sleep and the car expects them to. A device that keeps a bus awake **drains the
12 V battery and throws unrelated fault codes**. This firmware flushes, closes its file and idles
when traffic stops (`IDLE_FLUSH_MS` / `IDLE_SLEEP_MS`), then reopens a new file when traffic
returns.

---

## Firmware

Build/flash (Teensy 4.1):
```bash
arduino-cli compile --fqbn teensy:avr:teensy41 .
arduino-cli upload  -p <port> --fqbn teensy:avr:teensy41 .
```
Defaults: **CAN1 @ 500 kbit/s**, **CAN2 @ 100 kbit/s** (set in `config.h` — confirm the real
rates with your logic analyser before trusting them).

**Design choices that matter:**
- **128 KB DMAMEM ring buffer**, drained to SD in **16 KB blocks**, never per-frame. Small
  synchronous writes cause 10–100× write amplification and widen the window where a power cut
  lands mid-write.
- **Append-only** binary records — no database, no journaling, nothing to corrupt.
- **ID census with a changed-bit mask** kept live in RAM: for every ID it tracks *which bits have
  ever changed*. Constant bits are structure; moving bits are signals.

USB console: `i` status · `d` census · `c` clear census · `s` start/stop · `f` flush ·
`n` new file · `m` remount SD · `?` help

---

## Decoder

```bash
python canlog.py stats      CAN_0001.BIN          # ID census, rates, live-bit masks
python canlog.py candidates CAN_0001.BIN          # rank bytes that behave like sensors
python canlog.py extract    CAN_0001.BIN --id 1A0 --byte 4 --width 16 --endian BE \
                            --scale 0.01 --out soc.csv
python canlog.py dump       CAN_0001.BIN --id 1A0 --limit 50
```
Stdlib only — runs here, on the Pi, or on a Luckfox.

### `candidates` is the useful one
It scores every byte (and every 16-bit BE/LE pair) on how much it **behaves like a real sensor
value**, and separates four classes:

| Behaviour | Treatment |
|---|---|
| Smooth / slowly varying | **ranked high** — these are your signals |
| **Monotonic drift** | **ranked high, tagged `monotonic`** — SoC draining, temp rising, fuel dropping |
| Free-running counter | demoted (`counter (delta +1 100%)`) |
| Random each frame | demoted (`random-looking (checksum/crypto?)`) |
| Constant | excluded |

> **Verified against a synthetic log with known ground truth.** A bug was found and fixed during
> that test: slow **monotonic** signals originally scored **zero**, because every delta was
> identical so they looked like counters. **That would have buried battery State-of-Charge —
> the single signal most worth finding on a PHEV.** The fix distinguishes them properly: a real
> counter changes on *every* sample, a slow sensor is mostly still.

### Workflow for decoding the 330e
1. Log a drive, a charge session, and a cold start — separately.
2. Run `candidates` on each.
3. `extract` the top hits to CSV and correlate against what the car was doing (SoC on the
   dash, charging state, engine on/off).
4. Write confirmed findings into a `signals.md` next to the logs.

**Signals worth hunting on a 330e:** HV battery SoC, usable kWh, EV range, charge power/state,
**ICE start/stop transitions** (deceptively valuable — on a PHEV "engine running" isn't a simple
flag and correlates with nearly everything), electric/combustion split, regen, HV pack temps,
and 12 V rail health.

⚠️ Some powertrain data on an F30 is on **FlexRay (10 Mbit/s)**, not CAN. If a signal can't be
found on any CAN bus, that's likely why — decoding FlexRay is a Zynq PL job, not a Teensy one.

---

## Log format
```
header : "CANLOG2\n" + u32 bus1_baud + u32 bus2_baud        (16 bytes)
record : u32 t_us | u8 bus | u8 lenflags | u32 id | data[len]
         lenflags: bits0-3 = dlc, bit4 = extended, bit5 = remote
```
The decoder tolerates a truncated final record — expected after a power cut, not an error.
