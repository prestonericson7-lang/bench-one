# MESH_ARCHITECTURE

**The ESP-NOW fleet layer: a Teensy 4.1 with no radio commanding a fleet of ESP32-S3 sensor
nodes through a single ESP32-S3 relay.**

This document is the reproduction manual. Everything in it is either read out of the source in
this repository or measured on the three physical boards. Where a number appears, it was
observed; where a design decision appears, the reason is given.

---

## 0. Read this before you flash anything

> ### The ESP32-S3 build detail that will cost you an evening
>
> Compile **and** upload with the **same fully-qualified board name**, and that name must
> include `CDCOnBoot=cdc`:
>
> ```
> esp32:esp32:esp32s3:PartitionScheme=min_spiffs,CDCOnBoot=cdc
> ```
>
> Two independent traps live in that one line.
>
> **Trap 1 — `CDCOnBoot`.** On the ESP32-S3, `Serial` is a compile-time alias. With
> `CDCOnBoot=cdc` it is `HWCDCSerial`, the native USB peripheral. Without it, `Serial` is
> `Serial0`, which is UART0 on **GPIO43/44**. Omit the option and the board flashes
> successfully, reboots, runs perfectly — and says nothing at all on the USB port, with no
> error anywhere. You will conclude the sketch crashed. It did not; you are listening to the
> wrong pins.
>
> **Trap 2 — cached builds.** `arduino-cli upload` can upload a **cached build produced under a
> different fqbn**. If you compile with the long fqbn and then upload with the short one, you
> may flash a stale binary built with different options and get symptoms that match neither
> source tree. Always pass identical fqbn strings to both commands, or use the single
> `compile --upload` form so there is only one string to get wrong.
>
> Both of these were paid for with real debugging time in this project.

---

## 1. The system

Three boards, three roles, two protocols, one measurement discipline.

| Board | Part | Port | MAC | Role |
|---|---|---|---|---|
| **BRAIN** | Teensy 4.1 (IMXRT1062, 600 MHz) | COM38 | — (no radio of any kind) | Policy. Decides what happens. |
| **HUB** | ESP32-S3 | COM27 | `28:84:85:54:E1:AC` | Mechanism. Moves packets, measures air time. |
| **NODE** | ESP32-S3 + GY-521 IMU | COM29 | `1C:DB:D4:46:D5:0C` | Leaf. Reads a sensor, reports what it saw. |

The governing principle, stated once and applied everywhere below:

> **Mechanism lives on the radio. Policy lives on the brain.**

The hub knows *how* to move ESP-NOW packets. The brain decides *which* nodes matter, what they
should be doing, how much of their data it wants, and what a silent node means. The hub relays
node payloads verbatim and never rescales or summarises them. The brain — which has an FPU and
knows the configured full-scale range — is the first place raw sensor counts become physical
units.

### 1.1 Topology

```
                         ┌──────────────────────────────────────────┐
                         │  BRAIN — Teensy 4.1, IMXRT1062, 600 MHz  │
                         │  COM38  ·  no radio, none, at all        │
                         │  console + policy + DWT cycle counter    │
                         └────────────┬─────────────────────────────┘
                                      │
                       UART 921600 8N1, framed, CRC-16/MCRF4XX
                       shared/interop_protocol.h  ·  channel 0x04 = MESH
                                      │
              Teensy pin 1 (TX1) ─────┼────────────▶ ESP32-S3 GPIO18 (RX)
              Teensy pin 0 (RX1) ◀────┼───────────── ESP32-S3 GPIO17 (TX)
              Teensy GND        ──────┴───────────── ESP32-S3 GND
                       (TX goes to RX. Crossed. It is always this.)
                                      │
                         ┌────────────┴─────────────────────────────┐
                         │  HUB — ESP32-S3                          │
                         │  COM27  ·  28:84:85:54:E1:AC             │
                         │  ESP-NOW ch 1, mesh-only mode            │
                         │  roster · loss counting · ping stopwatch │
                         └────────────┬─────────────────────────────┘
                                      │
                         ESP-NOW, 802.11 ch 1, unencrypted
                    shared/mesh_protocol.h · packed structs, NO framing
                                      │
             ┌────────────────────────┼────────────────────────┐
             │                        │                        │
   ┌─────────┴──────────┐   ┌─────────┴──────────┐   ┌─────────┴──────────┐
   │ NODE 0             │   │ NODE 1  (unbuilt)  │   │ … up to 8 nodes    │
   │ ESP32-S3           │   │                    │   │ MESH_MAX_NODES = 8 │
   │ COM29              │   │                    │   │                    │
   │ 1C:DB:D4:46:D5:0C  │   │                    │   │                    │
   └─────────┬──────────┘   └────────────────────┘   └────────────────────┘
             │
       I2C @ 400 kHz  +  one interrupt line
             │
   SDA ── GPIO8   ┐
   SCL ── GPIO9   ├─  GY-521 breakout, WHO_AM_I = 0x70
   INT ── GPIO4   ┘   (an MPU-6500-family part, NOT a real MPU-6050 — see §4.3)
   VCC ── 3V3         (NOT 5 V — see §4.2)
   GND ── GND
   AD0 ── open        (I2C address 0x68; tie high for 0x69)
```

The broadcast address `FF:FF:FF:FF:FF:FF` is a fourth destination that is not drawn, because it
is not a board. One packet to it reaches every node on the channel simultaneously. That is the
difference between commanding a fleet and iterating a list.

---

## 2. Why there are two protocols

The two links in this system have opposite properties, so they get opposite protocols. Using one
protocol for both would mean either carrying dead weight on the radio or leaving the serial link
undefended.

| | UART leg (`interop_protocol.h`) | ESP-NOW leg (`mesh_protocol.h`) |
|---|---|---|
| Transport | raw byte stream | discrete packets |
| Packet boundaries | **none — you must invent them** | provided by the MAC |
| Integrity check | **none — you must add one** | 802.11 FCS; corrupt frames never delivered |
| Addressing | **none — one wire, two ends** | every packet carries the sender's MAC |
| Max payload | 1015 bytes | 250 bytes, hard radio limit |
| Therefore | START byte, 16-bit length, SEQ, FLAGS/CHAN/CMD, CRC-16/MCRF4XX, resynchronising parser | plain packed little-endian struct, nothing else |

### 2.1 What the UART framing buys

```
 ┌───────┬───────┬───────┬───────┬───────┬──────┬─────┬───────────┬───────┬───────┐
 │ START │ LEN_H │ LEN_L │  SEQ  │ FLAGS │ CHAN │ CMD │  PAYLOAD  │ CRC_H │ CRC_L │
 └───────┴───────┴───────┴───────┴───────┴──────┴─────┴───────────┴───────┴───────┘
   0xAA     1 B     1 B     1 B     1 B     1 B   1 B   0..1015 B    1 B     1 B
                  └──────── CRC-16/MCRF4XX covers this span ──────┘
                          └──────── LEN counts these ────────┘
```

- `START = 0xAA` is a resynchronisation landmark, not a delimiter — it is **not** escaped and
  occurs inside payloads constantly. A false start is rejected by the length check (only 3..1018
  accepted, killing ~98.5% of them two bytes in) and then by the CRC (a further 1 in 65536).
  Combined odds of a noise byte surviving as a frame: roughly 1 in 4.2 million.
- `SEQ` bit 7 is a **class** bit. `0x00–0x7F` is a request or the response echoing it;
  `0x80–0xFF` is unsolicited. This distinction does real work in the mesh layer — see §7.3.
- CRC-16/MCRF4XX, check value `0x6F91`.

### 2.2 Why the ESP-NOW protocol has none of that

Every one of those fields solves a property of a **raw serial stream**. ESP-NOW is not a raw
serial stream:

- It delivers **discrete packets of known length**. A start byte and a length field would be
  restating what `esp_now_send()`'s `len` argument already told the receiver.
- The **802.11 MAC already CRCs every frame** and silently discards corrupt ones. A payload CRC
  would be a second, weaker check on data that has already passed a stronger one.
- Each packet arrives with the **sender's MAC** in `esp_now_recv_info_t`. An address field would
  duplicate it.

So `mesh_protocol.h` is a set of `__attribute__((packed))` structs, little-endian (ESP32 native
on both ends), and nothing more. That is not laziness. **Recognising when NOT to add a layer is
as much of the job as knowing how to build one.** A framing layer here would cost bytes of air
time — the scarce resource under test — to solve problems that no longer exist.

### 2.3 What ESP-NOW does *not* give you, and what the protocol therefore still carries

| Missing | Consequence | The protocol's answer |
|---|---|---|
| Retransmission | A lost packet is gone. The send callback reports only whether the MAC got an ack. | `seq` — a 16-bit rolling counter, so loss is **counted, not guessed**. |
| Ordering guarantee | Packets can arrive out of order. | Same `seq`. |
| Payload above 250 bytes | Hard ceiling. | `MESH_MAX_SAMPLES = 16` → 204-byte worst case, with headroom on purpose. |
| Any clock relationship between boards | Two ESP32s have independent, undisciplined oscillators. | **Nothing in this system ever compares two clocks.** See §8 — this is the intellectual core. |

The 204-byte figure: 16 samples × 12 bytes + 12-byte header. It is deliberately *not* squeezed
up against 250. A payload that exactly fills the maximum leaves nowhere to add a field later
without a format break.

---

## 3. Message catalogue — the ESP-NOW leg (`mesh_protocol.h`)

Message type numbering is a debugging affordance: hub→node commands are `0x1x`, node→hub reports
are `0x2x`, so a packet travelling the wrong direction is obvious in a hex dump.

| Type | Name | Direction | Size | Payload |
|---|---|---|---|---|
| `0x10` | `MESH_MSG_CONFIG` | hub → node | 5 B | `MeshConfigMsg` |
| `0x11` | `MESH_MSG_PING` | hub → node | 5 B | `MeshPingMsg` |
| `0x12` | `MESH_MSG_IDENTIFY` | hub → node | 1 B | type byte only |
| `0x20` | `MESH_MSG_SAMPLE` | node → hub | 12 + 12·n B | `MeshSampleMsg` |
| `0x21` | `MESH_MSG_PONG` | node → hub | 5 B | `MeshPingMsg`, token echoed |
| `0x22` | `MESH_MSG_HELLO` | node → hub | 6 B | `MeshHelloMsg` |

### 3.1 `MeshSampleMsg` — the wire layout

All multi-byte fields are **little-endian** (ESP32 native, and the Teensy decoder byte-assembles
in that order).

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `type` | u8 | `0x20` |
| 1 | `mode` | u8 | which mode produced this packet |
| 2 | `seq` | u16 LE | rolling; a gap **is** packet loss |
| 4 | `t_first_us` | u32 LE | node clock at the first sample. **Node-relative only.** |
| 8 | `int_to_send_us` | u16 LE | node-local latency: data-ready IRQ → `esp_now_send()` |
| 10 | `count` | u8 | samples actually present, 1..16 |
| 11 | `dropped` | u8 | samples the node itself discarded since the last packet, saturating |
| 12 | `samples[]` | 12 B each | `int16 ax, ay, az, gx, gy, gz` |

Total on air: `12 + 12 × count`. One sample = **24 bytes**; a full 16-sample batch = **204 bytes**.

Three things in that table are load-bearing:

**`dropped` exists so a node that cannot keep up must SAY so.** Silently thinning a stream is
worse than leaving a visible gap, because nothing downstream can tell it happened.

**Samples are RAW COUNTS, never scaled.** Converting to g and °/s on the node would bake in an
assumed full-scale range, cost float maths in an interrupt-driven path, and throw away the exact
bits the sensor produced. The brain has the FPU and knows the configured range. Same principle
as forwarding raw 802.11 reason codes: send the fact, interpret at the top.

**`t_first_us` is never compared to anything on another board.** It is the node's own
`esp_timer_get_time()`. Its only legitimate uses are differencing against another `t_first_us`
from the *same* node, and being carried along as provenance.

### 3.2 The offsets are derived, and the compiler checks them

`mesh_protocol.h` defines `MESH_SAMPLE_OFF_*` next to the struct, and then proves them correct at
compile time with a negative-array-size static assertion:

```c
typedef char mesh_offsets_are_correct[
    (sizeof(MeshSample) == MESH_SAMPLE_STRIDE &&
     ((char *)&((MeshSampleMsg *)0)->int_to_send_us - (char *)0) == MESH_SAMPLE_OFF_LATENCY &&
     /* … */ ) ? 1 : -1];
```

**Why this exists.** A receiver cannot cast a received buffer to `MeshSampleMsg *`: the payload
sits at an arbitrary offset inside a larger frame, and a packed struct at an odd address is an
unaligned access — a fault or a silently wrong read on Cortex-M. So receivers decode byte by
byte, and byte-by-byte decoding needs offsets.

**The first set was hand-counted and wrong.** The Teensy decoder used 10 / 12 / 14 where the truth
is 8 / 10 / 12, because the field widths were mis-added by two. Every symptom was silent: the CRC
passed, the bounds checks passed, no counter moved. A live accelerometer on a desk reported
`a = 0 0 0, g = 0 0 0, lat = 1 us`. The only clue was that the numbers were not plausible — and
"the data looks wrong" is a far weaker signal than a failed assertion.

The fix is the rule: **never hand-count offsets into a packed struct defined in a header you
already share.** Derive them once, next to the definition, and make the compiler check them. If a
field is added, moved or resized, every board fails to *build* rather than quietly decoding
garbage.

---

## 4. Sensor-node wiring, for someone who has never built one

### 4.1 The connections

| GY-521 pin | ESP32-S3 pin | Notes |
|---|---|---|
| `VCC` | **3V3** | Not 5 V. See §4.2. |
| `GND` | `GND` | Must be common with the ESP32. |
| `SCL` | **GPIO9** | Default `Wire` SCL on this core. Bus runs at 400 kHz. |
| `SDA` | **GPIO8** | Default `Wire` SDA. |
| `INT` | **GPIO4** | Data-ready. Not optional — see §4.4. |
| `AD0` | open, or `GND` | I2C address `0x68`. Tie to 3V3 for `0x69`. |
| `XCL`, `XDA` | leave unconnected | Auxiliary I2C master, unused here. |

**Pull-ups:** the GY-521 breakout carries its own SDA/SCL pull-ups (typically 4.7 kΩ) referenced
to the module's `VCC` rail. Do not add more. With one module on a short bus this is correct as
shipped; if you hang several modules off one bus you will need to remove all but one set.

**Bus length:** keep it short — jumper-wire lengths. 400 kHz over a breadboard with long leads is
where I2C starts producing intermittent `endTransmission()` failures that look like a dead sensor.

### 4.2 3.3 V, not 5 V, and the reason is not obvious

The module's I2C pull-ups tie to **its own VCC**. Power the module from 5 V and those pull-ups
drive SDA and SCL to 5 V — straight into ESP32-S3 pins that are **not 5 V tolerant** — even
though no 5 V wire ever touches the ESP32 directly. The "3.3–5 V" marking on these boards refers
to what their on-board regulator will accept, not to what they do to your bus.

### 4.3 WHO_AM_I = 0x70 — your "MPU-6050" is not an MPU-6050

The board in this project is sold as a GY-521 / MPU-6050. Its `WHO_AM_I` register (`0x75`) reads
**`0x70`**. A genuine MPU-6050 reads `0x68`. `0x70` is an **MPU-6500-family part or clone**.

This matters and it does not matter, in that order:

- **It does not break anything here.** The register map used by this firmware — `PWR_MGMT_1`,
  `SMPLRT_DIV`, `CONFIG`, `GYRO_CONFIG`, `ACCEL_CONFIG`, `INT_PIN_CFG`, `INT_ENABLE`,
  `ACCEL_XOUT` — is compatible across the family. Everything in this project works.
- **It will break your assumptions if you don't know.** Do not write `if (who_am_i != 0x68) fail;`
  and do not copy datasheet values for offset registers, temperature scaling or the DMP from an
  MPU-6050 document and expect them to hold.

So the node **reports `WHO_AM_I` in every `HELLO`**, and the brain prints it:

```
[mesh] 1C:DB:D4:46:D5:0C is fw 1.0 caps=0x01 sensor=ok WHO_AM_I=0x70
```

The fleet never has to guess what silicon is at the far end. That is the general pattern: when a
part cannot be trusted to be what the label says, make the firmware *ask* and put the answer in
the identity message.

### 4.4 Why the INT pin is not optional

You can read this IMU by polling, and for displaying orientation that is fine. **It is useless
for measuring latency.** Polling timestamps the moment *you looked*, not the moment the sample
existed, so every measurement carries an unknown 0..interval of polling jitter — which at any
realistic loop rate swamps the air time we are trying to measure.

With `INT` wired, the data-ready interrupt timestamps the sample at the instant the sensor has
it, and `int_to_send_us` becomes a real, defensible number.

### 4.5 What the ISR does, and what it refuses to do

```c
static void IRAM_ATTR onDataReady(void)
{
    gIsrTimeUs   = (uint32_t)esp_timer_get_time();
    gSampleReady = true;
    gIsrCount++;
}
```

It records a timestamp and sets a flag. That is all. It does **not** touch I2C (a blocking bus)
and it does **not** call `esp_now_send()` — neither is safe from interrupt context, and doing
either would trade a precise measurement for an unstable radio. `loop()` does the work; the ISR
only captures *when*.

`loop()` snapshots the ISR timestamp with interrupts disabled before clearing the flag, so a
sample arriving mid-read cannot cause the wrong instant to be attributed to this packet.

### 4.6 Sensor configuration, and the two registers that interact

| Register | Value | Effect |
|---|---|---|
| `PWR_MGMT_1` (0x6B) | `0x00` | **Wake.** The part boots asleep and returns all zeros until this is cleared. This is the single most common bring-up mistake and it looks exactly like a dead sensor. |
| `CONFIG` (0x1A) | `0x03` | DLPF ≈ 44 Hz. **Also drops the internal rate from 8 kHz to 1 kHz.** |
| `GYRO_CONFIG` (0x1B) | `0x00` | ±250 °/s |
| `ACCEL_CONFIG` (0x1C) | `0x00` | ±2 g |
| `SMPLRT_DIV` (0x19) | `(1000 / rate_hz) − 1` | Divides the **1 kHz** rate set by `CONFIG`. |
| `INT_PIN_CFG` (0x37) | `0x00` | Active-high, push-pull, **pulsed** (not latched). |
| `INT_ENABLE` (0x38) | `0x01` | `DATA_RDY_EN` |

`CONFIG` and `SMPLRT_DIV` **interact**: the divisor divides 1 kHz only because the DLPF is
enabled. Set `CONFIG = 0` and the same `SMPLRT_DIV` silently gives you 8× the intended rate.

The interrupt is pulsed rather than latched deliberately. A latched interrupt stays asserted
until `INT_STATUS` is read, which is safer against a missed edge but costs an extra I2C
transaction per sample — and at 1 kHz that bus traffic starts to matter.

### 4.7 The node's own diagnostics, and the most misleading signal in the system

The node's 5-second heartbeat carries four numbers that exist because of one specific failure:

```
[node] up 30s ch=1 mode=0 irq=6059 sent=6058 fail=0 recv=2 sensor=ok(0x70) readfail=0 skipped=0
```

| Counter | Meaning |
|---|---|
| `irq` | data-ready interrupts seen |
| `sent` / `fail` | ESP-NOW send-callback outcomes |
| `sensor` | `ok` / `DOWN`, with the observed `WHO_AM_I` |
| `readfail` | `mpuRead()` returned false — the sensor is present on the INT line but unreadable on I2C |
| `skipped` | `loop()` saw a sample it could not act on because `gSensorOk` was false |

**The failure that motivated them.** After an ESP32 reset the node came up with its I2C
initialisation failing, so `gSensorOk` was false and the sampling block was skipped entirely. But
**the MPU does not reset when the ESP32 does.** It kept the sample-rate and interrupt
configuration written before the reset, and kept pulsing its data-ready line at exactly the
configured rate. The heartbeat showed `irq` climbing at a perfect 202 Hz while `sent` stayed
frozen at 0 — which reads like a *transmit* problem and is actually a *sensor* problem.

> A live interrupt from a sensor you cannot talk to is one of the most misleading signals in
> embedded work: the one number that looks healthiest is the one proving the sensor was
> configured **earlier**, not that it is readable **now**.

`skipped` is incremented *before* the sample is discarded, deliberately, so a node that is
interrupting but unreadable is distinguishable from one that is simply idle. Without that
counter the two states look identical from outside.

**The node also keeps trying.** Every status period, if `!gSensorOk || gReadFail > 0`, it re-runs
`mpuBegin()`:

```
[node] sensor unusable -- retrying init
[node] sensor recovered, WHO_AM_I=0x70
```

That costs a few I2C transactions every 5 s and turns a permanently dead node into one that
recovers by itself when a marginal connection settles or a brown-out clears. Going silent after a
transient fault is a design choice, and it is the wrong one.

### 4.8 Sampling modes

| Mode | Value | Behaviour | What you can measure with it |
|---|---|---|---|
| `MESH_MODE_ON_INTERRUPT` | `0x00` | One packet per IMU sample, sent at the data-ready IRQ. | Per-sample latency. Every packet carries its own `int_to_send_us`. Highest packet rate — which is the point when stress-testing the mesh. |
| `MESH_MODE_BATCHED` | `0x01` | Accumulate `batch` samples, send one packet. | Throughput and air-time efficiency. Per-sample latency is blurred into the batch interval: **you measure the batch, not the event.** |
| `MESH_MODE_IDLE` | `0x02` | Sample nothing. The node stays reachable and still answers PING and IDENTIFY. | Baseline. What the link costs with no traffic on it. |

Both data modes are implemented so the trade can be *measured* rather than argued about.

The node sends only the bytes actually used — `offsetof(samples) + count × 12` — not a fixed
204. A fixed-size send for a single sample would waste roughly 90% of the air time, and air time
is the resource under test.

---

## 5. The hub

The hub is a relay with a stopwatch and a roster. It has no opinions.

### 5.1 State it keeps per node (`MeshNode`)

| Field | Purpose |
|---|---|
| `mac[6]`, `used`, `alive` | roster slot, occupancy, liveness |
| `lastSeenMs` | drives the `MESH_NODE_TIMEOUT_MS = 5000` liveness window |
| `packets` | every packet from this node, **counted regardless of the stream divisor** |
| `lastSeq`, `seqValid` | previous sequence number |
| `lostPackets` | inferred from gaps in the **node's own** sequence numbers |
| `relayCount` | per-node phase counter for the stream divisor, so one chatty node cannot starve a quiet one out of its share |

`MESH_MAX_NODES = 8`, `MESH_RX_QUEUE_SIZE = 12`.

**Loss is inferred from the node's counter, not the hub's.** The hub cannot distinguish a packet
that was dropped from one that was never sent. The node's `seq` can.

### 5.2 The receive path, and why it queues

`onMeshRecv()` runs on the **WiFi task**. Building and transmitting a UART frame from there would
mean two tasks writing the TX buffer and `Serial1` concurrently — producing occasionally garbled
frames, only under load, which is the worst possible failure schedule. So the callback copies the
packet into a ring and returns; `loop()` drains one packet per pass and does the UART work.

Two things happen in the callback anyway, and both have a reason:

1. **A PONG's arrival time is captured immediately**, before queueing. Queueing it first would add
   loop latency to a number that is supposed to be air time.
2. **On overflow the NEWEST packet is dropped, not the oldest.** Sensor samples are a stream, not
   a set of state events; dropping the newest keeps the queue temporally contiguous, so a gap in
   the node's sequence numbers reads as one clean loss rather than a shuffle.

### 5.3 The send callback is deliberately empty

```c
static void onMeshSent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac; (void)status;
}
```

ESP-NOW's send status reports only whether the **MAC layer got an ack**. It does not mean the
application at the far end received, parsed or acted on anything. Treating it as delivery is a
classic overread. Real reachability is measured by the ping round trip.

### 5.4 The channel problem — the thing that actually bites

ESP-NOW peers **must share a WiFi channel**. That is fine until the hub also wants to be a WiFi
station, because then **the access point chooses the channel** and every node has to follow.

The failure mode is nasty and gives you nothing to debug with: everything initialises without
error, every `esp_now_send()` returns `ESP_OK`, and **not one packet is ever received**. There is
no error to read, because from the radio's point of view nothing is wrong — it transmitted, on
the channel it was told to use.

Two modes exist so the cost of coexistence can be measured rather than argued:

| Mode | Value | Behaviour |
|---|---|---|
| `IOP_MESH_MODE_ONLY` | `0x00` | No AP association. The hub pins the channel with `esp_wifi_set_channel()` and owns it outright. Deterministic; nothing else contends. **This is what all the measurements below were taken in.** |
| `IOP_MESH_MODE_WITH_WIFI` | `0x01` | Station mode stays up. If associated, the AP dictates the channel and the requested one is **ignored**. |

`MESH_INIT_RESP` therefore returns **the channel actually in use**, not the one requested. A node
told the wrong number transmits into silence.

Both hub and node pin the channel the same way — set promiscuous, set channel, clear promiscuous
— because without that the radio sits on whatever channel it last used.

### 5.5 A broadcast peer is registered at init

`FF:FF:FF:FF:FF:FF` is added as an ESP-NOW peer during `meshBegin()`. This is what lets a node be
*heard* before the hub knows its MAC; without it, discovery is a chicken-and-egg problem. Note
that even broadcast must be registered as a peer before `esp_now_send()` will accept it.

### 5.6 Autostart, and why a radio must be testable without a brain

| Constant | Value | What it does |
|---|---|---|
| `MESH_AUTOSTART_MS` | `8000` | If the brain has not sent `MESH_INIT_REQ` within 8 s of boot, the hub brings the mesh up itself on `MESH_AUTOSTART_CHAN = 1`. |
| `MESH_PING_AUTOSTART_MS` | `4000` | Ping train starts 4 s after the **first node is discovered** — not after boot. |
| `MESH_PING_AUTOSTART_COUNT` | `30` | Round trips per train. |
| `MESH_PING_REPEAT_MS` | `60000` | Re-characterise the air every minute. `0` = once. |

Normally the brain owns these decisions, because channel and mode are policy. But **a radio that
cannot be exercised without a brain attached is untestable exactly when you most need to test
it**: during bring-up, when the UART may not be working yet. That is not hypothetical — this
feature was added while the link to the Teensy was down with a broken jumper, and without it the
mesh could not have been measured at all.

The brain's later `MESH_INIT_REQ` overrides whatever autostart chose, so this is a fallback, never
a competing authority.

The ping delay is measured from **discovery**, not boot, for two reasons: before the first node
exists there is nothing to ping, and the settle time keeps the first trains from measuring the
node's boot sequence. It reschedules whether or not a node was available, so a node that appears
later still gets characterised instead of the autostart silently giving up forever.

---

## 6. The MESH command table (UART, channel `0x04`)

All commands live on `IOP_CHAN_MESH = 0x04`. Multi-byte integers in these payloads are
**big-endian**, matching the rest of `interop_protocol.h`. (The ESP-NOW payloads carried *inside*
`MESH_DATA` remain little-endian — the hub relays them verbatim and does not touch them.)

| Cmd | Name | Dir | Payload |
|---|---|---|---|
| `0x30` | `MESH_INIT_REQ` | brain → hub | `[channel]` `[mode]` |
| `0x31` | `MESH_INIT_RESP` | hub → brain | `[state]` `[actual_channel]` `[hub_mac 6]` — 8 B |
| `0x32` | `MESH_PEER_REQ` | brain → hub | `[op]` `[mac 6]` — op `0`=add, `1`=remove |
| `0x33` | `MESH_PEER_RESP` | hub → brain | `[status]` `[peer_count]` — 2 B |
| `0x34` | `MESH_SEND_REQ` | brain → hub | `[mac 6]` `[raw ESP-NOW bytes…]` |
| `0x35` | `MESH_SEND_RESP` | hub → brain | `[status]` — 1 B |
| `0x36` | `MESH_DATA` | hub → brain, **unsolicited** | `[src_mac 6]` `[node payload verbatim…]` |
| `0x37` | `MESH_PING_REQ` | brain → hub | `[mac 6]` `[count]` — count `0` means 20 |
| `0x38` | `MESH_PING_RESP` | hub → brain, **response *or* event** | 14 B, see below |
| `0x39` | `MESH_STATUS_REQ` | brain → hub | empty |
| `0x3A` | `MESH_STATUS_RESP` | hub → brain | 11 B, see below |
| `0x3B` | `MESH_NODE_EVENT` | hub → brain, **unsolicited** | `[event]` `[mac 6]` — 7 B |
| `0x3C` | `MESH_STREAM_REQ` | brain → hub | `[divisor]` — 1 B |
| `0x3D` | `MESH_STREAM_RESP` | hub → brain | `[status]` `[divisor]` — 2 B |
| `0x3E` | `MESH_NODES_REQ` | brain → hub | empty |
| `0x3F` | `MESH_NODES_RESP` | hub → brain | `[count]` then count × `[mac 6][alive][packets 4 BE]` — 11 B each |

### 6.1 `MESH_PING_RESP` (0x38) — 14 bytes

| Byte | Field | Width |
|---|---|---|
| 0 | `status` | u8 — `IOP_MESH_STATUS_OK` if any pong arrived, `ERROR` if none |
| 1–2 | `sent` | u16 BE |
| 3–4 | `recv` | u16 BE |
| 5–8 | `min_us` | **u32 BE** |
| 9–11 | `mean_us` | **u24 BE** |
| 12–13 | `max_us` | **u16 BE** |

Note the asymmetric widths — both encoder and decoder agree on them, but they are a real limit:
`max_us` saturates the field above **65,535 µs**. The largest max ever observed here was 7,260 µs,
so there is an order of magnitude of headroom, but a badly congested channel could overflow it.

### 6.2 `MESH_STATUS_RESP` (0x3A) — 11 bytes

| Byte | Field |
|---|---|
| 0 | `state` — `OFF 0x00` / `READY 0x01` / `ERROR 0xFF` |
| 1 | `channel` actually in use |
| 2 | `mode` — `MESH_ONLY 0x00` / `WITH_WIFI 0x01` |
| 3 | `peers` (roster slots used) |
| 4 | `alive` (peers within the liveness window) |
| 5–8 | `rx_count` u32 BE — **every ESP-NOW packet received, ungated by the stream divisor** |
| 9 | `rx_dropped`, saturating at 255 |
| 10 | `stream` divisor currently in effect |

Byte 10 was **appended, not inserted**. A brain built against the 10-byte version bounds its walk
with `payload_len` and simply never looks there. Growing a response at the tail is the one shape
of protocol change that needs no version negotiation.

### 6.3 Status and event constants

```
IOP_MESH_STATUS_OK       0x00     IOP_MESH_EVT_NODE_SEEN   0x01
IOP_MESH_STATUS_BUSY     0x01     IOP_MESH_EVT_NODE_LOST   0x02
IOP_MESH_STATUS_NO_NODE  0x02
IOP_MESH_STATUS_ERROR    0xFF     IOP_MESH_PEER_ADD        0x00
                                  IOP_MESH_PEER_REMOVE     0x01
```

These status constants exist because the hub was originally reusing `IOP_BLE_STATUS_OK` in its
mesh ping reply. The value was right (`0x00`) and it worked — which is exactly what made it worth
fixing before the two ranges diverged.

**And then the half-finished migration bit us in the same session.** The constants were
introduced and the ping reply converted, but `MESH_PEER_RESP` and `MESH_SEND_RESP` were left
sending `IOP_MESH_STATE_READY` (`0x01`) while the newly written brain checked for
`IOP_MESH_STATUS_OK` (`0x00`). Every successful peer-add and every successful send reported
**"FAILED"** to the operator. Nothing crashed, no NACK was raised, no counter moved: the two
boards simply disagreed about what `0x01` meant.

Two lessons, both worth the space. **Introducing a constant is not the change — converting every
site is the change**, and a partial conversion is strictly worse than none because it looks done.
And **a status byte is a shared vocabulary between two independently compiled programs**; the
compiler checks neither side against the other, so the only defences are converting all at once
and testing the *success* path. Failure paths were tested religiously in this project and it still
shipped a bug that only appears when things go right.

---

## 7. Three protocol behaviours worth understanding before you use it

### 7.1 The stream is a subscription, and it defaults to OFF

`MESH_STREAM_REQ` carries one divisor byte:

| Divisor | Meaning |
|---|---|
| `0` | **Off. The default.** The hub still tracks nodes, counts every packet and detects loss — it just forwards nothing. |
| `1` | Every packet. The firehose, for when the brain genuinely wants it. |
| `N` | Every Nth packet. A 200 Hz sensor at divisor 20 gives the brain a 10 Hz view for a display or a control loop, while the hub keeps counting all 200. |

**The bug this exists to correct.** The first version of the hub relayed every ESP-NOW packet to
the brain the instant it arrived, unconditionally, with no way to stop it. The brain never asked
for the stream and could not decline it. Measured cost with one node at 200 Hz: worst-case UART
round trip went from **299 µs to 1001 µs**, and **97.8%** of the brain's console output became a
single "unknown channel" line repeated 198 times a second.

**Why it was wrong, not merely expensive.** "Should this telemetry go to the brain, and how much
of it" is a statement about what the application needs. That is policy, and policy belongs on the
brain. The relay code already refused to rescale or summarise node payloads — correctly, and with
a comment saying so — but the decision to send them *at all* is the same kind of decision, and
that half was never questioned. **It is easy to get the visible half of a principle right and miss
the invisible half.**

**Decimation happens on the hub, and only forwarding is gated.** Marking the node alive, counting
its packet and checking its sequence number for loss all still run at full rate. So `mesh status`
reports exact totals at any divisor, and the brain can watch a 200 Hz sensor at 10 Hz without
losing the ability to say how many packets were actually lost. Decimating *measurement* along with
*delivery* would be the easy mistake here. Decimating at the brain instead would spend the wire,
both parsers and the brain's loop time to produce the same answer.

### 7.2 Node events are never gated

`MESH_NODE_EVENT` — a node appearing or going silent — always reaches the brain, whatever the
divisor. This is the distinction the protocol draws everywhere:

> **A rare fact the brain must not miss is an EVENT, and events are pushed.
> A continuous stream is SUBSCRIBED to.**

A node dying while the stream is off is exactly when the brain most needs telling.

### 7.3 Events and responses are not interchangeable — and both roster paths are needed

An **unrequested** measurement is published as an event (`SEQ` bit 7 set). A **requested** one is
a response echoing the request's `SEQ`. The hub tracks which with `gPingAuto`. Sending an
autostarted ping result as a response would hand the brain a reply carrying a sequence number it
never issued — precisely the stale-response case `SEQ` exists to catch, manufactured by us.

Correspondingly, `MESH_NODES_REQ` (`0x3E`) exists *even though* `NODE_SEEN` events already
announce every node. **Events only reach a brain that was listening when they fired.** A Teensy
that reboots, or is reflashed while the hub keeps running, has missed every announcement and has
no way to learn a single MAC. That is not hypothetical: `mesh ping` failed here with "no live node
known" while the hub was happily tracking a live node.

> Events keep a listening brain current, cheaply. A poll lets a brain that missed them recover the
> truth. **A system with only one of the two is broken for whoever arrives late.**

---

## 8. The control surface

Typed at the Teensy console. A `<target>` is an index from `mesh nodes`, a full MAC, or `all` —
the ESP-NOW broadcast address, one packet reaching every node at once.

| Command | What it does |
|---|---|
| `mesh` | What the brain *believes*. Sends nothing. |
| `mesh nodes` | **Polls** the hub for the roster and MACs. Prints the index→MAC mapping. |
| `mesh status` | Hub state and exact air-side totals. |
| `mesh watch on\|off` | Print live samples as they arrive. |
| `mesh mode <t> <interrupt\|batched\|idle> [rate_hz] [batch]` | Reconfigure a node, or every node. |
| `mesh id <t>` | Ask a node who it is (fw version, caps, `WHO_AM_I`). |
| `mesh ping <t> [count]` | ESP-NOW round trip, hub-clock timed. **One node only.** |
| `mesh send <t> <hex…>` | Raw bytes to a node. The escape hatch. |
| `mesh init [ch] [wifi]` | Start the mesh on a channel. |
| `mesh peer add\|remove <t>` | Edit the hub roster directly. |
| `mesh stream off\|all\|N` | How much node data is forwarded to the brain. |

Design notes on the surface itself:

- `mesh` with no argument reports the brain's own view **without sending anything**. A status
  command that perturbs the system is a status command you cannot trust.
- Targets accept an **index** because typing `1C:DB:D4:46:D5:0C` at 2 a.m. is how you configure the
  wrong node. `mesh nodes` prints the mapping, and the index is stable for as long as the roster is.
- `mesh ping` **refuses `all`**. N pongs sharing one token cannot be attributed, so a broadcast
  ping would produce a number that is not a measurement of anything. Ping each index and compare.
- Live sample printing is gated **twice**: by `mesh watch`, and by whether the USB host is actually
  draining the console. Without the second guard a 200 Hz print blocks `loop()` for 120 ms at a
  time and destroys the very latency this project measures.

### 8.1 Worked example — command in, remote reaction out

Captured across three boards on one PC clock. This is the whole point of the system: a board with
no radio changing the behaviour of a board it cannot talk to.

**Put node 0 to sleep and watch the fleet notice:**

```
BRAIN  >>> mesh mode 0 idle
NODE       [cfg] mode=2 rate=200Hz batch=16                 <- the remote board obeyed
HUB        [mesh] nodes=0/1 ... 1C:DB:D4:46:D5:0C LOST
BRAIN      [mesh] node 1C:DB:D4:46:D5:0C LOST (silent past the liveness window)
```

The node is not dead — it is idle, and still answers PING and IDENTIFY. But it stopped sending
samples, so after `MESH_NODE_TIMEOUT_MS = 5000` the hub declared it lost and **pushed an event**.
The brain learned about it without asking. That is §7.2 working.

**Bring every node back with a single broadcast packet:**

```
BRAIN  >>> mesh mode all interrupt 200                       <- ONE broadcast packet
NODE       [cfg] mode=0 rate=200Hz batch=16
HUB        [mesh] nodes=1/1
```

`all` resolves to `FF:FF:FF:FF:FF:FF`. One `MESH_SEND_REQ` over the UART becomes one ESP-NOW
packet on the air, and every node on the channel reconfigures simultaneously. Adding a
hundredth node would not add a byte to that command.

**Turn the firehose off and confirm counting continues:**

```
BRAIN  >>> mesh stream off
HUB        [mesh] rx=12256 dropped=0 stream=0 relayed=1856 held=10290
```

`rx` keeps climbing while `relayed` is frozen: **the hub counts everything and forwards nothing.**
`held` (12256 − 1856 = 10290) is exactly the packets counted but not relayed. Loss statistics
remain exact. That is §7.1 working.

**A decoded live sample reaching the brain:**

```
a = -216  456  17340    g = 467  158  -82    lat = 507 us
```

`az ≈ 17340` counts at ±2 g full scale is roughly 1 g — the board is sitting flat on a desk. That
plausibility check is how we know the byte-offset decode is correct, and its absence
(`a = 0 0 0, lat = 1 us`) is how we found out it was wrong the first time.

---

## 9. Timing — why no measurement in this system compares two clocks

**This is the intellectual core of the project. Everything else is plumbing.**

### 9.1 The naive approach, and exactly why it fails

The obvious way to measure "how long did that take" is: stamp the packet with the node's clock,
receive it on the hub, subtract the hub's clock.

```
        node clock ──▶  t_send = 1,204,551 µs        ┐
                                                     ├──  hub − node = "latency"?
        hub clock  ──▶  t_recv = 8,391,077 µs        ┘
```

That number is meaningless, and worse, it is *plausibly* meaningless.

The two boards have **independent crystal oscillators with no discipline between them**. Each
`esp_timer_get_time()` counts from its own board's boot, at its own board's frequency. The
difference between them is dominated by:

1. **An unknown constant offset** — the two boards booted at different instants, so the
   difference starts at some arbitrary value in the seconds-to-minutes range.
2. **An unbounded linear drift** — the crystals differ by tens of ppm. At 20 ppm, the two clocks
   separate by 20 µs every second, 72 ms per hour. Against a real air time of ~1.6 ms, the drift
   term overtakes the signal in **80 seconds**.

You would get a number. It would be stable-looking over any short window. It would slowly
change over a long one, in a way that looks like "the system is degrading" and is actually just
the crystals separating. You could plot it, average it, put error bars on it, and it would still
be measuring nothing but oscillator mismatch. **That is the worst kind of measurement: one that
is wrong but does not look wrong.**

The empirical tell is that the drift has no relationship to load. Halve the packet rate, and a
real latency measurement moves; a clock-offset measurement keeps drifting at exactly the same
rate, because it was never measuring latency.

Fixing it properly means clock synchronisation — PTP, or a shared hardware trigger line, or an
NTP-disciplined estimate with a servo — and that is a substantial subsystem with its own error
budget. This project does not need it, because it never asks the question that requires it.

### 9.2 The discipline: every figure is a single-clock delta

Three measurements. Each one starts and stops on **one board's clock**, so no agreement between
oscillators is required or assumed. An offset that is common to both the start and stop reads
cancels exactly, and drift over a sub-millisecond window is unmeasurable.

```
   NODE clock only          HUB clock only              TEENSY clock only
   ───────────────          ──────────────              ─────────────────
   IMU data-ready IRQ       hub: send PING, t0          Teensy: send frame, DWT snapshot
        │                        │                            │
        │ int_to_send_us         │  ping RTT                  │  UART round trip
        │                        │  (halve for one way)       │  (ARM_DWT_CYCCNT delta)
        ▼                        ▼                            ▼
   esp_now_send() called    hub: PONG arrives, t1        Teensy: response parsed

   both reads: esp_timer   both reads: esp_timer        both reads: ARM_DWT_CYCCNT
   on the NODE             on the HUB                   on the TEENSY
```

| Measurement | Clock | Start | Stop | Field / mechanism |
|---|---|---|---|---|
| Node-side latency | node `esp_timer_get_time()` | data-ready IRQ (in the ISR) | `esp_now_send()` called | `int_to_send_us`, u16, carried in every sample packet |
| Air time | hub `esp_timer_get_time()` | `esp_now_send(PING)` | PONG recognised in the receive callback | `MESH_PING_REQ` / `MESH_PING_RESP`; one way ≈ RTT / 2 |
| UART leg | Teensy `ARM_DWT_CYCCNT` | frame handed to `Serial1` | matching response parsed | DWT cycle counter, free-running at the core clock |

**Those three sum to the end-to-end path, and no term requires two clocks to agree.**

### 9.3 Why each one is measured the way it is

**`int_to_send_us` is taken in the ISR** because the interrupt edge is the only instant at which
the sample provably exists. A polled read timestamps when *you looked*. The ISR does nothing but
read the timer and set a flag — any I2C or radio work inside it would be measuring the ISR, not
the sensor. The delta is computed as unsigned arithmetic (`nowUs - gFirstUs`) so a 32-bit
`esp_timer` wrap is harmless, and saturated at 65,535 to fit the u16 field.

**The ping token is echoed verbatim.** Without it, a *late* pong from a previous round could be
counted as a *fast* one for the current round, and the mean would improve as the link got worse.
This is the same stale-response hazard that `SEQ` solves on the UART, appearing again on a
different transport — which is a good sign the abstraction is real.

**The node echoes a PING from inside the receive callback**, before doing anything else. Any work
done before replying would be measured by the hub as air time and would corrupt the one number
this message exists to produce.

**The hub reads its stopwatch in the receive callback, not in `loop()`.** Everything else queues
and is handled by `loop()`; the PONG timestamp cannot, because loop latency would be added to a
figure that is supposed to be air time.

**A ping timeout is not fatal.** `gPingDeadline = now + 200 ms`; on expiry the train simply
continues. Loss is *measured*, not treated as an error, because a 30-round-trip train that aborts
on the first drop tells you nothing about a link that drops 3%.

**The Teensy uses `ARM_DWT_CYCCNT` rather than `micros()`** because `micros()` has 1 µs
granularity and the round trips being measured are in the hundreds of microseconds — quantisation
error alone would be ~0.25% and would hide exactly the tail behaviour that matters. The DWT
counter ticks once per CPU clock at 600 MHz and is already running on this core.

### 9.4 What the discipline costs, and what it buys

**Costs:** you cannot say "this specific sample was captured at absolute time T". There is no
global timeline. Correlating events across boards to better than a round-trip time is not
possible.

**Buys:** every number in §10 is defensible without a single assumption about clock discipline.
There is no synchronisation subsystem to build, tune, verify or debug — and no drift term hiding
in a plot. When a figure changes, something in the system changed.

For this project that is the right trade, and the right trade is visible only because the question
was asked: *what do I actually need to know?* The answer was "how long does each leg take", not
"when in absolute time did this happen" — and the first question does not need synchronised
clocks.

---

## 10. Measured performance

All figures below were captured on the three physical boards described in §1, mesh in
`IOP_MESH_MODE_ONLY` on channel 1, IMU configured for 200 Hz.

### 10.1 UART leg — Teensy → hub → Teensy

Teensy-local, `ARM_DWT_CYCCNT`. 300 samples, hub console on USB CDC, mesh stream **OFF**.

| Metric | Value |
|---|---|
| Samples | 300 |
| Lost | **0** |
| Min | 408.940 µs |
| Mean | 435.939 µs |
| Max | 635.400 µs |
| Std dev | 42.393 µs |

For reference, 12 bytes at 921600 8N1 is **130.2 µs** of pure wire time. The remaining ~300 µs is
the two ends: parse, dispatch, build, transmit, on both boards.

Compare with §7.1: the same measurement with the ungated relay running had a worst case of
**1001 µs**. The subscription mechanism is worth roughly 366 µs of worst-case latency.

### 10.2 ESP-NOW round trip — hub-clock timed

| Run | Round trips | Loss | Min | Mean | Max | One-way ≈ |
|---|---|---|---|---|---|---|
| A | 50 | **0%** | 2181 µs | 3248 µs | 6824 µs | ~1624 µs |
| B | 30 | **0%** | 2254 µs | 3359 µs | 7260 µs | ~1680 µs |

The two runs agree to within 3.4% on the mean, which is the useful result: the measurement is
repeatable. Note the spread — max is roughly 3× min. That is the 802.11 medium, not the firmware:
ESP-NOW contends for the channel like any other WiFi transmission.

**The ESP-NOW leg is about 7.5× the UART leg.** The radio dominates end-to-end latency by an order
of magnitude, which is exactly the kind of fact this measurement exists to establish and exactly
the kind of fact that guessing gets wrong.

### 10.3 End-to-end chain — four independent counters on three boards

The most valuable number in this document, because it is four numbers that agree.

| Stage | Board | Counter | Rate | Errors |
|---|---|---|---|---|
| IMU data-ready interrupt | node | `gIsrCount` | **201.96 Hz** | — |
| ESP-NOW packets sent | node | `gSent` | **202.0 Hz** | fail = 0 |
| ESP-NOW packets received | hub | `gMeshRx` | **201.9 Hz** | dropped = 0 |
| UART frames parsed | brain | frame counter | **201.6 Hz** | discarded = 0 |

**All four agree within 0.2%.** Nothing is being lost anywhere in the chain: sensor to interrupt
to radio to relay to serial to parser.

The IMU is configured for 200 Hz and free-runs **0.98% fast**. That is normal for an
uncompensated MEMS part. It is also precisely why **the node timestamps its own samples instead
of the hub assuming a rate**: a hub that computed sample times from "200 Hz" would accumulate
2 ms of error every second and never know.

### 10.4 UART parser robustness

After **53,530 frames** at 921600 baud over jumper wire:

| Counter | Value |
|---|---|
| Bad CRC | **0** |
| Impossible length | **0** |
| Oversize | **0** |
| Frame timeouts | **0** |
| Bytes discarded | **0** |

A clean short link at 921600 does not corrupt frames. The framing layer is not there because the
wire is bad — it is there because *when* the wire goes bad, or a board resets mid-frame, the
parser recovers instead of desynchronising permanently.

### 10.5 Stream decimation, observed

```
[mesh] rx=12256 dropped=0 stream=0 relayed=1856 held=10290
```

| Counter | Value | Meaning |
|---|---|---|
| `rx` | 12256 | every ESP-NOW packet received — **ungated** |
| `dropped` | 0 | receive-queue overflows |
| `stream` | 0 | divisor: off |
| `relayed` | 1856 | forwarded to the brain before the stream was turned off |
| `held` | 10290 | counted, not forwarded (12256 − 1856 = 10290 ✓) |

### 10.6 Derived wire arithmetic

From the figures above, not separately measured:

- One on-interrupt sample on the air: `12 (header) + 12 (one sample) = ` **24 bytes** of
  `MeshSampleMsg`.
- Relayed as `MESH_DATA`: `6 (MAC) + 24 = 30` payload bytes, plus 3 body-header bytes and 6 frame
  overhead = **39 bytes** on the UART.
- At 200 packets/s: `200 × 39 × 10 bits = 78,000 bit/s`, or **8.5% of 921,600 baud**.
- The same 200 samples/s in `batched` mode with `batch = 16`: 12.5 packets/s × (6 + 204 + 9) =
  219 bytes = **~2.7 kB/s**, against 7.8 kB/s on-interrupt — a **~2.9× reduction in wire
  traffic**, paid for by blurring per-sample latency into an 80 ms batch window.

That last line is the whole reason both modes exist, expressed as a number instead of an opinion.

---

## 11. Build one yourself

### 11.1 Bill of materials

| Qty | Part | Notes |
|---|---|---|
| 1 | Teensy 4.1 | The brain. Any board with a spare hardware UART and a cycle counter works; the DWT timing code is Cortex-M specific. |
| 2 | ESP32-S3 devkit | One hub, one node. `-N8R8` variants need non-default board settings — see §11.3. |
| 1 | GY-521 / MPU-6050 breakout | Expect `WHO_AM_I = 0x70`. See §4.3. |
| — | Jumper wires | Short. Three for the UART, five for the IMU. |
| — | 2× USB cables | Plus a third if you want all three consoles at once, which you do. |

Everything else is optional. There is no level shifter, no crystal, no external pull-up, no
antenna work.

### 11.2 Wire it

**Brain ↔ hub, three wires:**

```
Teensy pin 1 (TX1)  ─────▶  ESP32-S3 hub GPIO18 (RX)
Teensy pin 0 (RX1)  ◀─────  ESP32-S3 hub GPIO17 (TX)
Teensy GND          ──────  ESP32-S3 hub GND
```

TX goes to RX. If nothing ever arrives at either end and both boards print their banners happily,
this is the first thing to check — it is the failure that looks like a software problem.

**Node ↔ IMU, five wires:** §4.1. `SDA=GPIO8`, `SCL=GPIO9`, `INT=GPIO4`, `VCC=3V3`, `GND=GND`.

The hub does **not** need the IMU. The node does **not** need the UART. Each board is
independently testable, which is deliberate.

### 11.3 Flash it

> Re-read §0 first. `CDCOnBoot=cdc`, and identical fqbn on compile and upload.

**Both ESP32-S3 boards:**

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:PartitionScheme=min_spiffs,CDCOnBoot=cdc" \
  --upload -p COM27 esp32_wireless_bridge

arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:PartitionScheme=min_spiffs,CDCOnBoot=cdc" \
  --upload -p COM29 esp32_sensor_node
```

Using the single `compile --upload` form is recommended precisely because there is then only one
fqbn string, and it cannot disagree with itself or pick up a cache entry from a different one.

For an `-N8R8` devkit also set Flash Size **8 MB**, PSRAM **OPI PSRAM**, Partition Scheme
**8 MB with spiffs** — the defaults assume 4 MB and no PSRAM.

**Teensy 4.1:**

```bash
arduino-cli compile --fqbn teensy:avr:teensy41 --upload -p COM38 teensy_master_controller
```

If that fails with `Unable find Teensy Loader`, bypass the Loader's localhost IPC hop entirely and
talk HalfKay straight to the bootloader over raw HID:

```bash
arduino-cli compile --fqbn teensy:avr:teensy41 --output-dir build teensy_master_controller
python tools/teensy_halfkay_flash.py --port COM38 build/teensy_master_controller.ino.hex
```

`--port` opens that port at 134 baud first, which makes a running sketch jump into the bootloader
— no button press needed.

**Set the node's hub MAC before flashing the node**, or accept the default and let discovery sort
it out. In `esp32_sensor_node.ino`:

```c
static const uint8_t HUB_MAC[6] = { 0x28, 0x84, 0x85, 0x54, 0xE1, 0xAC };
```

This is a **bootstrap address, not a hard dependency**. The node learns whoever last spoke to it
(`memcpy(gHubMac, info->src_addr, 6)` in the receive callback), so it can be adopted by a
different hub without reflashing. A node that only ever answers a hardcoded MAC cannot join a
fleet it wasn't built for.

Read your own hub's MAC off the board rather than guessing it: the hub prints it at boot, and
`mesh init` returns it in `MESH_INIT_RESP`.

### 11.4 Verify it, in order

Each step isolates one layer. Do not skip ahead; a failure at step 4 means something different
depending on whether step 3 passed.

**1 — Node console (COM29), immediately after flashing:**

```
[node] my MAC : 1C:DB:D4:46:D5:0C
[imu]  MPU-6050 ok, WHO_AM_I=0x70, 200 Hz, data-ready IRQ enabled
[mesh] ESP-NOW up on channel 1
[node] running
```

If you see nothing at all: §0, `CDCOnBoot`. If you see the banner but
`[imu] NOT FOUND` — check `SDA=GPIO8`, `SCL=GPIO9`, `VCC=3.3V`, common ground.

Then every 5 s:

```
[node] up 30s ch=1 mode=0 irq=6059 sent=6058 fail=0 recv=2 sensor=ok(0x70) readfail=0 skipped=0
```

`irq` climbing at ~202/s proves the interrupt line. `sent` tracking `irq` with `fail=0` proves the
radio is transmitting. **`irq` climbing while `sent` stays at 0 does not mean the radio is
broken** — check `sensor=`, `readfail=` and `skipped=` first. See §4.7.

**2 — Hub console (COM27).** Within 8 s of boot the autostart fires, and within 4 s of the node's
first packet the ping train runs — **with no brain attached at all**:

```
[mesh] no MESH_INIT from the brain after 8000 ms -- autostarting on channel 1 ...
[mesh] new node 1C:DB:D4:46:D5:0C
[mesh] autostart ping train: 30 round trips to 1C:DB:D4:46:D5:0C, timed on this board's clock
[mesh] ping done (autostart): 30/30, min 2254us mean 3359us max 7260us (one way ~1679us)
```

At this point the entire radio layer is verified and characterised, and you have not yet powered
the Teensy. That is the point of the autostart.

If the ping reports `0/30`: the two radios are on different channels. Everything will have
initialised without error and every send will have returned `ESP_OK`. See §5.4.

**3 — Brain console (COM38):**

```
>>> mesh status
[mesh] hub says: ready, ch 1, mode mesh-only, 1 peer(s) 1 alive, rx=… stream=0
```

That proves the UART leg, the frame format and the CRC in both directions. If it times out,
check TX↔RX crossing before anything else.

**4 — Learn the roster and address a node:**

```
>>> mesh nodes
[mesh] fleet roster from the hub: 1 node
       [0] 1C:DB:D4:46:D5:0C  alive  packets=12256
       address them by index: 'mesh mode 0 idle', 'mesh ping 0'
```

**5 — Prove end-to-end telemetry:**

```
>>> mesh stream all
>>> mesh watch on
[mesh] 1C:DB:D4:46:D5:0C seq=4471 a=  -216    456  17340  g=   467    158    -82 lat=507us
```

Check `az`. Flat on a desk at ±2 g it should read roughly 1 g — about 17,340 counts here. If you
get `a = 0 0 0 lat=1us` from a live sensor, your byte offsets are wrong; see §3.2, it has happened
before.

**6 — Prove the control path, both directions:**

```
>>> mesh mode 0 idle           # node console prints [cfg] mode=2, hub declares it LOST in 5 s
>>> mesh mode all interrupt 200 # one broadcast packet; node returns, hub prints nodes=1/1
>>> mesh stream off             # rx keeps climbing, relayed freezes
```

If all six pass, you have reproduced the system.

---

## 12. Failure modes, and what each one actually means

| Symptom | Real cause | Fix |
|---|---|---|
| ESP32-S3 completely silent on USB after a successful flash | `CDCOnBoot=cdc` missing; `Serial` is UART0 on GPIO43/44 | §0. Recompile with the full fqbn. |
| Flashed binary does not match the source | `upload` used a cached build from a different fqbn | Use identical fqbn strings, or `compile --upload`. |
| Every `esp_now_send()` returns `ESP_OK`, nothing is ever received | Radios on different channels | §5.4. Check the channel in `MESH_INIT_RESP` — it reports the **actual** channel, not the requested one. |
| IMU reads all zeros, WHO_AM_I responds fine | `PWR_MGMT_1` not cleared; part is still asleep | §4.6. |
| `irq` climbs at a perfect 202 Hz but `sent` stays 0 — looks like a dead radio | The ESP32 reset; the MPU did **not**, and keeps pulsing INT with its pre-reset configuration while I2C init fails | §4.7. Read `sensor=`, `readfail=`, `skipped=`. The node retries init every 5 s on its own. |
| Sample rate is 8× what you asked for | `CONFIG` DLPF disabled, so `SMPLRT_DIV` divides 8 kHz not 1 kHz | §4.6. The two registers interact. |
| `a = 0 0 0`, `lat = 1 us` from a live sensor | Hand-counted struct offsets | §3.2. Derive them; let the static assert check them. |
| Every successful mesh operation reports "FAILED" | Status constants converted on one side only | §6.3. Test the success path. |
| `mesh ping` says "no live node known" while the hub tracks one | The brain rebooted and missed the `NODE_SEEN` events | `mesh nodes` — that is what the poll is for. §7.3. |
| Node reports rising `dropped` | The node cannot keep up at the configured rate | Lower `rate_hz`, or switch to `batched` mode. |
| Brain console flooded, UART latency tripled | Stream divisor set to 1 at 200 Hz | `mesh stream 20`, or `off`. §7.1. |
| UART works but nothing arrives in either direction | TX not crossed to RX | §11.2. |

---

## 13. Source map

| File | Contains |
|---|---|
| `shared/mesh_protocol.h` | ESP-NOW message structs, wire offsets, static assertion, timing rationale |
| `shared/interop_protocol.h` | UART framing, CRC-16, SEQ classes, MESH command IDs `0x30–0x3F`, stream-divisor rationale |
| `esp32_sensor_node/esp32_sensor_node.ino` | IMU bring-up, data-ready ISR, mode handling, node-side ESP-NOW |
| `esp32_wireless_bridge/esp32_wireless_bridge.ino` | Hub: roster, receive queue, relay + decimation, ping stopwatch, autostart, `handleMeshFrame()` |
| `teensy_master_controller/teensy_master_controller.ino` | Brain: console verbs, `handleMeshData()` decode, DWT round-trip timing |
| `docs/PLATFORM_BASELINE.md` | The ungated-relay regression, measured |
| `docs/BRINGUP_LOG.md` | Toolchain and hardware bring-up, including the Teensy Loader IPC failure |
