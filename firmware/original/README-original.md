# Teensy 4.1 ↔ ESP32-S3 — a brain, a radio, and a fleet

A binary framing protocol between a Teensy 4.1 and an ESP32-S3 over a direct UART at 921600 baud,
and a second protocol behind it over which that ESP32 commands a fleet of other ESP32s by ESP-NOW.
Written from scratch — no COBS, no Protobuf, no SLIP, no existing serial library, no mesh library.

**The Teensy 4.1 is the brain. The ESP32-S3 is a radio — and now a hub, holding a fleet of remote
radios on the far side of it.** That sentence decides every design question in this project, so it
is worth stating before anything else:

> **Mechanism lives on the radio. Policy lives on the brain.**

The ESP32 knows how to drive an 802.11 MAC — start a scan, attempt a join, read a disconnect
reason, move an ESP-NOW packet to a peer. It reports what happened. It does not decide what
happens next: not how many times to retry, not which network to prefer, not whether a failure
matters, not which nodes matter, not what a silent node means. Those are application decisions,
and the application runs on the Teensy.

Four consequences you can see in the code:

1. **The radio reports raw facts alongside its summary.** `WIFI_CONNECT_RESP` carries a friendly
   status byte *and* the raw 802.11 reason code. The summary is convenience; the raw code is
   truth. Reason 15 and reason 202 both mean "wrong password" to the radio, but 202 is an
   outright rejection while 15 is a handshake timeout — which is also what a *correct* password
   produces on a marginal link. Only the brain has the context to tell those apart.
2. **The radio can speak without being asked.** Connections drop and chips brown out on the
   radio's schedule, not the brain's. `SEQ` bit 7 marks unsolicited frames.
3. **The radio is replaceable.** Nothing in the frame format mentions Espressif. Auth modes and
   status codes are translated to our own enums at the radio's edge.
4. **The hub relays; it does not interpret.** Node payloads cross the hub verbatim — never
   rescaled, never summarised. The brain, which has an FPU and knows the sensor's configured
   full-scale range, is the first place raw sensor counts become physical units.

---

## Status

| Milestone | Scope | State |
|---|---|---|
| 1 | Framing, CRC, parser, PING/PONG, error paths | **Implemented** — verified on hardware |
| 2 | WiFi scan | **Implemented** — verified on hardware |
| 3 | WiFi connect | **Implemented** — verified on hardware |
| 4 | Protocol **v2**: channel byte, CRC-16/MCRF4XX, FLAGS, SEQ classes | **Implemented** — 53,530 frames over jumper wire, 0 errors of any class |
| 5 | **MESH channel `0x04`** — ESP-NOW fleet: discovery, liveness, remote configuration, hub-timed ping, subscription control | **Implemented and verified across three boards** |
| 6 | BLE — channel `0x02`, commands `0x20`–`0x28` and `IOP_CAP_BLE` reserved | **Not started** |
| 7 | OTA | **Not started** |

**Protocol v2** is a deliberate, one-time wire break over v1. It adds a **channel byte** (so a
second radio technology costs one channel value rather than a renumbering), replaces CRC-8 with
**CRC-16/MCRF4XX**, adds a **FLAGS byte** (fragmentation and truncation had nowhere to live), and
splits **SEQ** into a request class and an unsolicited class so a *lost event becomes visible*.
The four changes landed together because a deferred wire break costs triple later. A v1 board and
a v2 board **cannot** interoperate — the header length and CRC width differ — and both sketches
print the version and CRC check value at boot and compare them on the first frame of a session, so
a half-upgraded pair is diagnosed in English rather than discovered an hour later.

An optional **0.96" SSD1306 OLED** on the Teensy shows link health with no PC attached.

All three sketches compile clean against the toolchains installed on this machine
(`teensy:avr 1.62.0`, `esp32:esp32 3.0.7`). The protocol core is verified by a native test
harness — see [Verification](#verification).

---

## The platform

Three boards, two protocols, one direction of authority.

```
  ┌──────────────┐   UART 921600 8N1    ┌──────────────┐   ESP-NOW ch 1   ┌──────────────┐
  │  Teensy 4.1  │  interop_protocol.h  │  ESP32-S3 #1 │ mesh_protocol.h  │  ESP32-S3 #2 │
  │    BRAIN     │◄────────────────────►│     HUB      │◄────────────────►│     NODE     │
  │              │  pin1→GP18, GP17→pin0│   (radio)    │                  │  + IMU       │
  └──────────────┘                      └──────────────┘                  └──────────────┘
       COM38                                 COM27                             COM29
    600 MHz M7                        28:84:85:54:E1:AC                 1C:DB:D4:46:D5:0C
```

| Role | Board | Port | Identity |
|---|---|---|---|
| Brain | Teensy 4.1 (IMXRT1062, 600 MHz) | COM38 | No radio of any kind |
| Hub / radio | ESP32-S3 | COM27 | MAC `28:84:85:54:E1:AC` |
| Sensor node | ESP32-S3 + GY-521 IMU | COM29 | MAC `1C:DB:D4:46:D5:0C`, `WHO_AM_I = 0x70` |

The brain has no radio. Everything wireless in this system happens because the Teensy asked for it
in a framed, CRC-checked sentence, or because a radio chose to speak up.

---

## Hardware

### Brain ↔ hub — the UART link

```
   Teensy 4.1  (brain)                        ESP32-S3  (radio / hub)
   ┌──────────────────┐                      ┌──────────────────┐
   │                  │                      │                  │
   │   pin 1  (TX1) ──┼──────────────────────┼─→ GPIO18 (RX)    │
   │   pin 0  (RX1) ←─┼──────────────────────┼── GPIO17 (TX)    │
   │            GND ──┼──────────────────────┼── GND            │
   │                  │                      │                  │
   └──────────────────┘                      └──────────────────┘
        USB to PC                                 USB (power / debug)
     (Serial Monitor)
```

| | Teensy 4.1 | ESP32-S3 |
|---|---|---|
| Port | `Serial1` (LPUART6) | `Serial1` |
| TX | pin 1 | GPIO17 |
| RX | pin 0 | GPIO18 |
| Baud | 921600, 8N1 | 921600, 8N1 |
| Logic | 3.3 V | 3.3 V |

**TX goes to RX — crossed, not straight through.** Wiring TX→TX produces perfect silence with no
error, because nothing is electrically wrong; both boards are simply talking into their own ears.

**Both boards are 3.3 V.** Connect them directly. Do not insert a 5 V level shifter or an Arduino
Uno — 5 V on an ESP32-S3 or Teensy 4.1 input damages the pin.

### The sensor node — a second ESP32-S3 with an IMU

The node has no wired connection to anything in the diagram above. It reaches the brain only by
radio, through the hub.

```
      GY-521 / IMU module          ESP32-S3 #2  (node)
      -----------------            -----------
      VCC  ──────────────────────► 3.3 V   (NOT 5 V — see below)
      GND  ──────────────────────► GND
      SCL  ──────────────────────► GPIO9   (default Wire SCL)
      SDA  ──────────────────────► GPIO8   (default Wire SDA)
      INT  ──────────────────────► GPIO4   (data-ready — this is what makes timing honest)
      AD0  ── open or GND ───────► I²C address 0x68 (tie high for 0x69)
```

| Signal | ESP32-S3 pin |
|---|---|
| `SDA` | GPIO8 |
| `SCL` | GPIO9 |
| `INT` | GPIO4 |
| I²C clock | 400 kHz |

**`WHO_AM_I` reads `0x70`, and that is not an MPU-6050.** A genuine MPU-6050 answers `0x68`;
`0x70` is an MPU-6500-family part or a clone, despite the module being sold as a GY-521/MPU-6050.
The register map used here is compatible, so everything works — but **do not assume a board marked
"GY-521" contains the chip the listing claims.** The node reads `WHO_AM_I` and reports it in every
`MESH_MSG_HELLO`, so the brain always knows what it is actually talking to. A sensor that
identifies itself honestly upward is worth more than one that is labelled correctly on the silk.

**Why the `INT` pin is not optional.** You can read this sensor by polling, and for displaying
orientation that is fine. It is useless for measuring latency: polling timestamps the moment *you
looked*, not the moment the sample existed, so every measurement carries an unknown 0..interval of
jitter — which at any realistic loop rate swamps the air time we are trying to measure. With `INT`
wired, the data-ready interrupt timestamps the sample at the instant the sensor has it, and the
node's reported `int_to_send_us` becomes a real, defensible number.

**What the ISR does, and what it refuses to do.** It records a timestamp and sets a flag. That is
all. It does not touch I²C (a blocking bus) and it does not call `esp_now_send` — neither is safe
from interrupt context, and doing either would trade a precise measurement for an unstable radio.
`loop()` does the work; the ISR only captures *when*.

**3.3 V, not 5 V — same trap as the OLED below.** The module's I²C pull-ups tie to *its own* VCC,
so a 5 V-powered module drives SDA and SCL to 5 V straight into pins that are not 5 V tolerant,
even though no 5 V wire ever touches the ESP32.

### Two hardware notes worth acting on

**Pull-ups on the RX lines.** When one board is held in reset its TX pin floats, and a floating
input is decoded as a continuous BREAK — usually delivered as a stream of `0x00` bytes. A 10 kΩ
resistor from each RX line to 3.3 V holds the idle state high and makes a resetting peer look
like silence instead of garbage. The parser survives either way, but silence is easier to read on
a scope.

**Power.** The ESP32 draws ~300 mA peaks when the transmitter keys up. The 330 µF + 100 nF
already built into this project is there for exactly that. If the radio reports
`BROWNOUT` in its `RADIO_READY` frame, the rail is sagging and no amount of protocol debugging
will help.

### Optional: status display on the Teensy

A 0.96" SSD1306 128×64 OLED, I²C address `0x3C`. Set `DISPLAY_ENABLED 0` in the sketch and the
whole feature compiles out — no library, no RAM, no I²C traffic.

| OLED pin | Teensy 4.1 pin |
|---|---|
| `GND` | GND |
| `VCC` | **3.3 V** |
| `SCL` | **19** |
| `SDA` | **18** |

Pins 18/19 are LPI2C1's only pins on a Teensy 4.1 (verified in the core's `WireIMXRT.cpp`) and
are clear of `Serial1` on pins 0/1. `Wire1` is SDA 17 / SCL 16 and `Wire2` is SDA 25 / SCL 24 if
you need the layout elsewhere.

**Power it from 3.3 V, not 5 V.** The module's I²C pull-ups tie to *its own* VCC, so a 5 V module
drives SDA and SCL to 5 V straight into pins that are not 5 V tolerant — even though you never
ran a 5 V wire to the Teensy. The "3.3–5 V" marking on these modules refers to their regulator,
not to what they do to your bus.

**0.96" is SSD1306; 1.3" is SH1106.** They answer at the same address and accept the same
commands, so I²C cannot tell them apart — but the SH1106 has 132 columns of RAM behind a
128-pixel panel. Drive one with the other's driver and you get a 2-pixel shift with a garbage
stripe down one edge.

### Why GPIO17 and GPIO18 on the ESP32-S3

Most S3 GPIOs are freely mappable, but several are spoken for, and using one produces a board
that will not boot, will not flash, or corrupts its own RAM:

| Pins | Why to avoid |
|---|---|
| 0, 3, 45, 46 | Strapping pins, sampled at reset — a pull changes boot mode |
| 19, 20 | Native USB D− / D+ |
| 26–32 | SPI flash |
| 33–37 | Octal PSRAM on `-N8R8` / `-N16R8` modules |
| 43, 44 | Default UART0 — the serial monitor port on most boards |

17 and 18 are clear of all of these on every common S3 module. If your board breaks out different
pins, change `LINK_TX_PIN` / `LINK_RX_PIN`; nothing else cares. The node's GPIO4/8/9 are clear of
the same list, which is why they were chosen.

---

## The fleet

The mesh layer is where the brain stops talking to *a* radio and starts commanding *radios*.

```
                      one framed, CRC-16'd request                  one 802.11 packet
   ┌──────────────┐    ────────────────────────►    ┌───────────┐   ─────────────────►  ┌────────┐
   │  Teensy 4.1  │                                 │ ESP32-S3  │                       │ node 0 │
   │    BRAIN     │        UART 921600 8N1          │    HUB    │      ESP-NOW ch 1     ├────────┤
   │              │  ◄────────────────────────      │           │   ◄─────────────────  │ node 1 │
   │  policy      │    relayed payloads + events    │ mechanism │    samples / pongs    ├────────┤
   └──────────────┘                                 └───────────┘                       │  ...   │
      no radio           interop_protocol.h            8 peers        mesh_protocol.h    └────────┘
                                                                    broadcast = all at once
```

### Two protocols, deliberately different

| | `shared/interop_protocol.h` | `shared/mesh_protocol.h` |
|---|---|---|
| Transport | UART, a raw byte stream | ESP-NOW, discrete packets |
| Framing | START byte, length, CRC-16, resynchronising parser | **none** |
| Byte order | big-endian on the wire | packed little-endian structs |
| Addressing | implicit (two endpoints) | the sender's MAC, from the 802.11 header |
| Integrity | our CRC-16/MCRF4XX | the 802.11 MAC CRC, already there |

Every reason `interop_protocol.h` has a start byte, a length, a CRC and a resynchronising parser is
a property of a **raw serial stream**: no packet boundaries, no integrity check, no addressing.
ESP-NOW has all three built in. Re-implementing framing on top would add bytes and complexity to
solve problems that no longer exist. **Recognising when *not* to add a layer is part of the
design.**

What ESP-NOW does *not* give you, and what `mesh_protocol.h` therefore still has to: no
retransmission (hence a sequence number, so loss is **counted, not guessed**), no ordering
guarantee, a hard 250-byte payload ceiling, and **no clock relationship between boards**.

### Never compare two free-running clocks

There is no timestamp comparison anywhere in the mesh layer, and that is the most important
decision in it. Stamping a packet on the node and subtracting the hub's clock on arrival produces
a number dominated by an unknown, drifting oscillator offset — plausible-looking and wrong, the
worst kind of measurement. Every figure here is a **delta measured on a single clock**:

| Measurement | Clock | What it is |
|---|---|---|
| `int_to_send_us` | node only | data-ready IRQ → `esp_now_send()`. Pure node-side latency. |
| ESP-NOW round trip | hub only | hub sends, node echoes, hub stops the timer. Halve it for one-way air time. |
| UART round trip | Teensy only | DWT cycle counter, nanosecond resolution. |

Those three sum to the end-to-end path, and no term requires two boards to agree on time.

### The control surface

Console verbs on the Teensy. A `<target>` is an index from `mesh nodes`, a full MAC, or `all` —
and `all` is the ESP-NOW broadcast address `FF:FF:FF:FF:FF:FF`, **one packet reaching every node
at once**.

| Verb | What it does |
|---|---|
| `mesh` | what the brain currently *believes* |
| `mesh nodes` | **POLL** the hub for the roster and MACs |
| `mesh status` | hub state and exact air-side totals |
| `mesh watch on\|off` | print live samples as they arrive |
| `mesh mode <t> <interrupt\|batched\|idle> [rate_hz] [batch]` | change what a **remote board does** |
| `mesh id <t>` | ask a node who it is — firmware version, capabilities, `WHO_AM_I` |
| `mesh ping <t> [count]` | ESP-NOW round trip, hub-clock timed. One node only. |
| `mesh send <t> <hex…>` | raw bytes to a node — bring up a new kind of node without reflashing this one |
| `mesh init [ch] [wifi]` | start the mesh on a channel; add `wifi` to coexist with a station connection |
| `mesh peer add\|remove <t>` | edit the hub's ESP-NOW roster |
| `mesh stream off\|all\|N` | how much node data is forwarded to the brain |

`mesh nodes` is a **poll, not a cache read**, and it is needed after a Teensy reset for the same
reason `status` exists on the WiFi side: node-seen events only reach a brain that was already up.

### Evidence that the control path is real

Captured across three boards on one PC clock with `tools/capture_baseline.py`:

```
BRAIN >>> mesh mode 0 idle
NODE      [cfg] mode=2 rate=200Hz batch=16              ← the remote board obeyed
HUB       [mesh] nodes=0/1 ... 1C:DB:D4:46:D5:0C LOST
BRAIN     [mesh] node 1C:DB:D4:46:D5:0C LOST            (silent past the liveness window)

BRAIN >>> mesh mode all interrupt 200                   ← ONE broadcast packet
NODE      [cfg] mode=0 rate=200Hz batch=16
HUB       [mesh] nodes=1/1

BRAIN >>> mesh stream off
HUB       [mesh] rx=12256 dropped=0 stream=0 relayed=1856 held=10290
```

That last line is the whole subscription design in one row: `rx` keeps climbing while `relayed` is
frozen. **The hub counts everything and forwards nothing.**

A live IMU sample decoded at the brain:

```
a=-216 456 17340   g=467 158 -82   lat=507us
```

`az ≈ 17340` counts is roughly 1 g, which is how we know the decode is correct — and it is worth
saying that an earlier version of that decoder used wire offsets of 10/12/14 where the truth is
8/10/12. Every symptom was silent: the CRC passed, the bounds checks passed, no counter moved, and
a live accelerometer reported `a = 0 0 0, g = 0 0 0` with a latency of 1 µs. The only clue was that
the numbers were not *plausible*. "The data looks wrong" is a far weaker signal than a failed
assertion, which is why the offsets are now derived in the header rather than hand-counted.

### The subscription — why the stream defaults to OFF

`IOP_CMD_MESH_STREAM_REQ` carries one divisor byte: `0` = off (**the default**), `1` = every
packet, `N` = every Nth. Two properties make this more than a throttle:

- **Decimation happens on the hub, and the hub keeps counting every packet regardless.** Packet-loss
  statistics stay exact at any divisor, so turning the firehose down costs you nothing you were
  measuring.
- **Node events are never gated.** A node appearing or going silent is pushed unconditionally. A
  rare fact the brain must not miss is an *event*; a continuous stream is *subscribed to*. **A node
  dying while the stream is off is precisely when the brain most needs telling.**

The hub tracks up to **8 peers** and declares a node LOST after **5000 ms** of silence.

> **Full treatment:** [`docs/MESH_ARCHITECTURE.md`](docs/MESH_ARCHITECTURE.md) — the fleet layer end
> to end: packet layouts, the discovery and liveness state machines, the broadcast path, and the
> reasoning behind each. [`docs/PLATFORM_BASELINE.md`](docs/PLATFORM_BASELINE.md) is the
> timestamped hardware capture that this section's numbers come from, including the three defects
> it found.

---

## Measured performance

Every number below came off hardware, not a model.

### UART round trip — 300 samples, 0 lost

Hub console on USB CDC, mesh stream off.

| min | mean | max | σ |
|---|---|---|---|
| 408.940 µs | **435.939 µs** | 635.400 µs | 42.393 µs |

For reference, 12 bytes at 921600 8N1 is **130.2 µs** of pure wire time.

### ESP-NOW round trip — hub-clock timed

| Run | Round trips | Loss | min | mean | max |
|---|---|---|---|---|---|
| 1 | 50 | 0% | 2181 µs | **3248 µs** | 6824 µs |
| 2 | 30 | 0% | 2254 µs | 3359 µs | 7260 µs |

One way is roughly **1624 µs** — about 3.7× the entire UART round trip. The air is the slow part,
and now that is a measured fact rather than an assumption.

### End-to-end telemetry chain — four independent counters on three boards

| Stage | Rate | Errors |
|---|---|---|
| IMU data-ready interrupt (node) | 201.96 Hz | — |
| ESP-NOW sent (node) | 202.0 Hz | `fail = 0` |
| ESP-NOW received (hub) | 201.9 Hz | `dropped = 0` |
| UART frames parsed (brain) | 201.6 Hz | `discarded = 0` |

All four agree within **0.2%**. The IMU is configured for 200 Hz and free-runs 0.98% fast, which is
normal for an uncompensated MEMS part — and is exactly why the node timestamps its own samples
instead of letting the hub assume a rate.

### UART parser counters after 53,530 frames at 921600 baud over jumper wire

| Counter | Value |
|---|---|
| Bad CRC | 0 |
| Impossible length | 0 |
| Oversize | 0 |
| Frame timeouts | 0 |
| Bytes discarded | 0 |

---

## Layout, and the one rule that matters

```
esp32-teensy-interop/
├── shared/
│   ├── interop_protocol.h          ← MASTER COPY. UART: brain ↔ hub.
│   └── mesh_protocol.h             ← MASTER COPY. ESP-NOW: hub ↔ nodes.
├── esp32_wireless_bridge/          the HUB
│   ├── esp32_wireless_bridge.ino
│   ├── interop_protocol.h          ← copy, kept in sync by the script
│   └── mesh_protocol.h             ← copy
├── esp32_sensor_node/              a NODE
│   ├── esp32_sensor_node.ino
│   └── mesh_protocol.h             ← copy
├── teensy_master_controller/       the BRAIN
│   ├── teensy_master_controller.ino
│   ├── interop_protocol.h          ← copy
│   └── mesh_protocol.h             ← copy
├── tools/
│   ├── sync_headers.py             ← run after every protocol edit
│   ├── host_fuzz.c                 ← native test harness
│   ├── capture_baseline.py         ← all three boards, one clock, one log
│   └── teensy_halfkay_flash.py     ← flash a Teensy without the Loader GUI
├── docs/
│   ├── MESH_ARCHITECTURE.md
│   ├── PLATFORM_BASELINE.md
│   ├── BRINGUP_LOG.md
│   └── protocol_reference.html
└── README.md
```

The Arduino IDE requires `foo.ino` to live in a folder named `foo/`, and only reliably compiles
headers sitting in that same folder. `#include "../shared/interop_protocol.h"` **does not work** —
it compiles on the ESP32 and fails on the Teensy, which is the worst possible outcome because the
ESP32 succeeding first gives false confidence. Symlinks need Developer Mode or an elevated prompt
on Windows, so they are not something to hand a beginner.

So each header is copied into every sketch folder that needs it, and a script keeps the copies
honest:

```bash
python tools/sync_headers.py
```

```bash
python tools/sync_headers.py --check
```

`--check` exits non-zero if a copy has drifted — drop it into CI or a pre-commit hook.

**Run it after every edit to either master header, and re-upload *every* board that holds a copy.**
A protocol change applied to one side only is worse than no change at all: the boards still talk,
they just disagree, and the symptom is a stream of CRC failures that looks like a wiring fault.

**The Teensy holds a copy of `mesh_protocol.h` even though it never speaks ESP-NOW**, and that is a
correction worth recording. The sync list originally excluded it, with the confident comment that
the brain "has no business holding a copy it cannot use." That was true when written and stopped
being true the moment the brain learned to *decode* node payloads relayed by the hub. Speaking a
transport and understanding the payload it carries are two different things. Without the header the
brain would have used a bare `0x20` for `MESH_MSG_SAMPLE` and kept working perfectly — right up
until someone renumbered the message types and every board agreed except the one holding a magic
number. Sharing the header makes that a compile error instead of a field bug.

There is a second safety net at runtime. The radio's `RADIO_READY` frame carries its protocol
version and CRC check value, and the brain compares them against its own on the first frame of
every session. A mismatched header is reported in plain English instead of being discovered an
hour later.

---

## Building

### Teensy 4.1
Board: **Teensy 4.1**. Defaults are fine. Optional:

| Setting | Effect |
|---|---|
| USB Type: **Dual Serial** | Adds `SerialUSB1`, to which every raw frame is mirrored as clean binary — a PC decoder can read the wire while a human reads English on the first port. Compiles to nothing when not selected. |
| CPU Speed | Any. Latency measurements read `F_CPU_ACTUAL` at runtime and scale correctly. |

### ESP32-S3 — both of them
Board: **ESP32S3 Dev Module** (or your specific board).

> **The default board settings will silently send your debug output to the wrong connector.**
>
> On the ESP32-S3, `Serial` is a *macro*, and what it expands to depends on **Tools → USB CDC On
> Boot**:
>
> | USB CDC On Boot | `Serial` becomes | Output appears on |
> |---|---|---|
> | **Disabled** (default) | `Serial0` = UART0 on GPIO43/44 | the port labelled **UART** |
> | Enabled | `HWCDCSerial` = native USB | the port labelled **USB** |
>
> Two ports, one macro, no diagnostic. If the boot banner never appears, try the other USB socket
> before you touch the wiring. The sketch prints which one this build is using, so the board tells
> you itself.

This setting is not merely cosmetic, and it cost real debugging time: a build flashed **without**
`CDCOnBoot=cdc` sends the hub's per-command debug lines to a 115200-baud hardware UART with nothing
draining it, so `write()` **blocks**. The UART round-trip mean went from 435.94 µs to 2261.35 µs
with a σ of 3894 µs, and the mesh relay was blamed for it for a while. It was the console. See
[`docs/PLATFORM_BASELINE.md`](docs/PLATFORM_BASELINE.md).

For an `-N8R8` devkit also set Flash Size **8 MB**, PSRAM **OPI PSRAM**, Partition Scheme
**8 MB with spiffs** — the defaults assume 4 MB and no PSRAM.

### The exact FQBN strings

Three boards, three commands. **Both ESP32 sketches need `CDCOnBoot=cdc`**; the Teensy has no such
option and needs none.

```bash
arduino-cli compile --fqbn teensy:avr:teensy41 teensy_master_controller
```

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" esp32_wireless_bridge
```

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" esp32_sensor_node
```

| Board | Role | FQBN | Port here |
|---|---|---|---|
| Teensy 4.1 | brain | `teensy:avr:teensy41` | COM38 |
| ESP32-S3 #1 | hub | `esp32:esp32:esp32s3:CDCOnBoot=cdc` | COM27 |
| ESP32-S3 #2 | node | `esp32:esp32:esp32s3:CDCOnBoot=cdc` | COM29 |

`arduino-cli` caches aggressively, and a compile can pass against stale objects after you have
broken a header. Use `--clean` when you are verifying rather than iterating.

---

## Flashing

### ESP32-S3

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --upload -p COM27 esp32_wireless_bridge
```

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --upload -p COM29 esp32_sensor_node
```

`CDCOnBoot=cdc` is not optional if you are plugged into the board's **native** USB port
(VID `0x303A` / PID `0x1001`). Without it `Serial` maps to UART0 on GPIO43/44 and you get a
successful flash with a completely silent monitor — and, as measured above, a link that appears to
have a latency problem it does not have. See the note under [Building](#building).

### Teensy 4.1

The normal path is `arduino-cli compile --fqbn teensy:avr:teensy41 --upload -p <port>`. If it
fails with `Unable find Teensy Loader`, that is a known-fragile localhost IPC hop between
`teensy_post_compile.exe` and the Loader GUI — see [the bring-up log](docs/BRINGUP_LOG.md#4-the-toolchain-fought-back-teensy-loader-ipc)
for the full diagnosis of what it is *not* (it is not the firewall, and not the Loader being down).

The reliable fallback bypasses that layer entirely and talks HalfKay straight to the bootloader
over raw HID:

```bash
python tools/teensy_halfkay_flash.py --port COM38 build/teensy_master_controller.ino.hex
```

`--port` makes it open that port at 134 baud first, which makes a running sketch jump into the
bootloader — no button press needed. Without `--port`, tap the button on the Teensy (a **single
short press**; holding it is the ESP32 gesture and does nothing here) and pass `--wait 60` to give
yourself time.

To produce the `.hex` without uploading:

```bash
arduino-cli compile --fqbn teensy:avr:teensy41 --output-dir build teensy_master_controller
```

---

## The frame

Protocol **v2**.

```
 ┌───────┬───────┬───────┬───────┬───────┬───────┬───────┬───────────┬───────┬───────┐
 │ START │ LEN_H │ LEN_L │  SEQ  │ FLAGS │ CHAN  │  CMD  │  PAYLOAD  │ CRC_H │ CRC_L │
 └───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────────┴───────┴───────┘
   0xAA     1 B     1 B     1 B     1 B     1 B     1 B    0..1015 B    1 B     1 B
          └──────────────────── CRC-16 covers this span ─────────────┘
                                  └──── LEN counts these, so LEN ≥ 3 ┘
```

| Field | Meaning |
|---|---|
| `START` | `0xAA`. Sync marker. Not escaped — see false starts below. |
| `LEN` | Big-endian uint16: bytes after `SEQ` up to but excluding the CRC. Never less than 3. |
| `SEQ` | Bit 7 is a **class** flag; bits 0–6 are a rolling counter within that class. `0x00`–`0x7F` = a request or the response echoing it; `0x80`–`0xFF` = **unsolicited**. |
| `FLAGS` | Frame-level modifiers, orthogonal to the command: `0x01` MORE (fragments follow), `0x02` TRUNCATED (the sender had more data than would fit). |
| `CHAN` | Which subsystem. **A receiver MUST route on `CHAN`.** |
| `CMD` | What this frame means, *within its channel*. |
| `CRC` | CRC-16/MCRF4XX over `LEN_H`..end of payload, high byte first. Check value **`0x6F91`**. |

Derived limits: max frame 1024 B, header overhead 6 B, max body 1018 B, max payload **1015 B**,
inter-byte timeout 50 ms.

`START` is excluded from the CRC because it is a marker, not data: if it were corrupted there would
be no frame to verify.

### Channels

| `CHAN` | Subsystem |
|---|---|
| `0x00` | Transport — PING/PONG, RADIO_READY, NACK |
| `0x01` | WiFi |
| `0x02` | BLE — reserved, not implemented |
| `0x03` | Data — reserved for bulk network traffic, not implemented |
| `0x04` | **Mesh** — the ESP-NOW fleet |

The channel byte is the single change that makes a second radio technology cheap. v1 had a flat
command space with ranges reserved by convention, and that does not survive BLE alone — scan,
advertise, connect, GATT read/write/notify, server and client. Espressif's own ESP-Hosted answers
this with a 4-bit interface selector in which Bluetooth consumes *zero* command codes, because HCI
frames are just a header followed by raw HCI bytes. Adding a third radio here costs one channel
value, not a renumbering.

### Why the SEQ class bit

v1 reserved the single value `0x00` for unsolicited frames. That worked, but it gave every
announcement the same sequence number — so **a lost event was undetectable**. Splitting the byte
gives requests 128 transaction ids and gives unsolicited frames their own rolling counter, so the
brain can *see* that it missed one. It also removes the `0xFF`→`0x00` wrap hazard structurally
instead of relying on a helper to remember to skip a value.

This matters because events genuinely do get lost: the radio's event queue can overflow before a
frame is ever built, the brain's receive ring can overrun, and the Teensy's UART can drop a byte in
hardware without setting any flag the core exposes.

### About that CRC

| | Parameters | Check(`"123456789"`) |
|---|---|---|
| **CRC-16/MCRF4XX** — what is implemented | poly `0x8408` reflected, init `0xFFFF`, no final XOR | **`0x6F91`** |

**Why 16 bits, and why this polynomial.** v1 used CRC-8 with poly `0x31`. That polynomial factors
as `(x+1)` × a degree-7 cofactor of order 127, so HD=4 held only to a **119-bit dataword** — about
10 payload bytes. Every longer frame was HD=2, and that included **every real `WIFI_CONNECT_REQ`**,
since a 5-character SSID with an 8-character passphrase is already 15 payload bytes. A two-bit
error in a credential frame could pass, the radio would try a wrong password, and the brain would
be told `WRONG_PASSWORD` for a password that was correct.

No better 8-bit polynomial exists — 119 bits is the maximum HD=4 length for *any* CRC-8 — so the
limit was the **width**, not the choice. CRC-16/MCRF4XX holds HD≥4 to 32,751 bits for one extra
byte, and it is the MAVLink polynomial, so logic-analyser decoders already know it.

**Name the parameters, not the label.** The original design brief called for "CRC-8/MAXIM
(Dallas/iButton)" *and* specified "poly 0x31, init 0x00, no reflect, no XOR out" — and **those
describe different algorithms** (real MAXIM-DOW reflects poly 0x31 and checks `0xA1`; the specified
parameters check `0xA2`; CRC-8/NRSC-5 checks `0xF7`). The parameters were followed rather than the
label, because parameters are unambiguous and labels are not. The same discipline applies now:
MCRF4XX is a reflected algorithm, and mixing its constants with an MSB-first table shape produces a
table that *still passes casual testing* and is wrong. Hand a third party the parameter row, not
the name.

The 256-byte table was generated and verified against an independent bitwise implementation. It was
never typed by hand.

### False starts: what happens when 0xAA appears in a payload

It does, constantly — `0xAA` is −86 dBm as a signed RSSI, a common value in a dense apartment.
Two filters reject an impostor:

1. **The LEN sanity check.** A random 16-bit value is accepted only in `3..1018`, so roughly 98.5%
   of false syncs die two bytes in, before any buffer is touched.
2. **The CRC.** Survivors must then produce a body whose trailing *two* bytes equal its own
   CRC-16: a further 1 in 65,536.

Combined, a random false start survives roughly **1 in 4.2 million** — against 1 in 16,000 for v1's
CRC-8, which was measured empirically at **1 in 17,614** over 4 MB of noise (52,843 false syncs, 3
survivors, each independently re-checked and each with a genuinely valid CRC — the parser never
lied; noise simply hit the lottery three times). And a survivor is still not obeyed: its `CHAN` and
`CMD` must both be implemented, or it is NACKed.

### The cost of a false start, corrected

An earlier version of this README claimed a rejected false start costs "at most one lost frame."
**That was wrong, and hardware proved it.** A false sync declaring an enormous length sent the
parser into its SKIP state to drain bytes that were never coming — swallowing every real frame that
arrived until the 50 ms timeout. It ate a live PING during bring-up. Twelve bytes of injected noise
containing a stray `0xAA` produced a declared length of **43,538**.

The parser now separates the two cases:

| Declared length | Judged as | Action |
|---|---|---|
| ≤ `IOP_MAX_SKIP_BYTES` (2048) | a real frame, too big for us — most likely a peer built with a larger `IOP_MAX_FRAME_SIZE` | drain it precisely, so the stream resumes cleanly |
| > `IOP_MAX_SKIP_BYTES` | not a frame any implementation can emit — our own builder refuses anything over 1015 payload bytes | report it and resync **immediately** |

So the real bound is: a false start costs at most **2048 bytes** (22 ms), comfortably inside the
inter-byte timeout, and normally far less. See [the bring-up log](docs/BRINGUP_LOG.md) for the
full account.

---

## Commands

### Channel `0x00` — transport

| CMD | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | `PING` | brain → radio | none |
| `0x02` | `PONG` | radio → brain | none |
| `0x04` | `RADIO_READY` | radio → brain, **unsolicited** | version, CRC check, reset reason, capabilities |
| `0xFE` | `NACK` | either | original cmd, error code |

### Channel `0x01` — WiFi

| CMD | Name | Direction | Payload |
|---|---|---|---|
| `0x10` | `WIFI_SCAN_REQ` | brain → radio | none |
| `0x11` | `WIFI_SCAN_RESP` | radio → brain | count, then per network: rssi, channel, enc, ssid_len, ssid, then total-found |
| `0x12` | `WIFI_CONNECT_REQ` | brain → radio | ssid_len, ssid, pass_len, pass |
| `0x13` | `WIFI_CONNECT_RESP` | radio → brain | status, IP×4, raw 802.11 reason |
| `0x14` | `WIFI_EVENT` | radio → brain, **unsolicited** | event type, raw reason, IP×4 |
| `0x15` | `WIFI_STATUS_REQ` | brain → radio | none |
| `0x16` | `WIFI_STATUS_RESP` | radio → brain | state, reason, RSSI, IP×4, events-dropped, caps, flags |

### Channel `0x04` — mesh

| CMD | Name | Direction | Payload |
|---|---|---|---|
| `0x30` | `MESH_INIT_REQ` | brain → hub | channel, mode |
| `0x31` | `MESH_INIT_RESP` | hub → brain | status, channel, hub MAC×6 |
| `0x32` | `MESH_PEER_REQ` | brain → hub | op (0 add / 1 remove), MAC×6 |
| `0x33` | `MESH_PEER_RESP` | hub → brain | status, peer count |
| `0x34` | `MESH_SEND_REQ` | brain → hub | MAC×6, node cmd, data… |
| `0x35` | `MESH_SEND_RESP` | hub → brain | status |
| `0x36` | `MESH_DATA` | hub → brain, **unsolicited** | a node's payload, **relayed verbatim** |
| `0x37` | `MESH_PING_REQ` | brain → hub | MAC×6, count |
| `0x38` | `MESH_PING_RESP` | hub → brain | status, sent, recv, min/mean/max µs |
| `0x39` | `MESH_STATUS_REQ` | brain → hub | none |
| `0x3A` | `MESH_STATUS_RESP` | hub → brain | state, channel, peers, rx count, drops |
| `0x3B` | `MESH_NODE_EVENT` | hub → brain, **unsolicited, never gated** | event, MAC×6 |
| `0x3C` | `MESH_STREAM_REQ` | brain → hub | divisor: 0 off, 1 every packet, N every Nth |
| `0x3D` | `MESH_STREAM_RESP` | hub → brain | status, divisor |
| `0x3E` | `MESH_NODES_REQ` | brain → hub | none |
| `0x3F` | `MESH_NODES_RESP` | hub → brain | count, then count × [MAC×6, alive, packets×4] |

### On the air — `mesh_protocol.h`

Hub-to-node commands are `0x1x`, node-to-hub reports are `0x2x`, purely so an unexpected direction
is obvious in a hex dump.

| Type | Name | Direction |
|---|---|---|
| `0x10` | `MESH_MSG_CONFIG` | hub → node: change mode / rate / batch |
| `0x11` | `MESH_MSG_PING` | hub → node: echo this back immediately |
| `0x12` | `MESH_MSG_IDENTIFY` | hub → node: who are you |
| `0x20` | `MESH_MSG_SAMPLE` | node → hub: IMU data |
| `0x21` | `MESH_MSG_PONG` | node → hub: ping echo |
| `0x22` | `MESH_MSG_HELLO` | node → hub: identity, on boot and on ask |

Sampling modes: `0x00` on-interrupt (one packet per sample — highest fidelity, every packet carries
its own node-side latency, and the highest packet rate, which is the point when stress-testing),
`0x01` batched (far fewer packets, but per-sample latency is blurred into a batch interval — you
measure the batch, not the event), `0x02` idle (sample nothing; the node stays reachable). Both are
implemented so the trade can be **measured rather than argued about**.

A sample packet carries 16 samples × 12 bytes plus a 12-byte header = 204 B, comfortably inside
ESP-NOW's 250-byte ceiling. A payload that exactly fills the maximum leaves nowhere to add a field
later without a format break.

**Samples are raw counts, not scaled.** Converting to g and deg/s on the node would bake in an
assumed full-scale range, cost float maths in an interrupt-driven path, and throw away the exact
bits the sensor produced. Same principle as forwarding raw 802.11 reason codes: send the fact,
interpret at the top.

**Error codes:** `0x01` unknown command · `0x02` bad CRC · `0x03` timeout · `0x04` busy ·
`0x05` payload too large · `0x06` malformed payload · `0x07` unknown channel · `0x08` not supported

**Capability bits:** `0x01` WiFi · `0x02` BLE · `0x04` mesh

**Encryption codes:** `0` open · `1` WEP · `2` WPA · `3` WPA2 · `4` WPA/WPA2 · `5` WPA3 ·
`6` WPA2/WPA3 *(ext)* · `7` enterprise *(ext)* · `0xFF` unknown *(ext)*

The extensions above `0x05` exist because real scans hit them constantly, and labelling a
transitional WPA2/WPA3 AP as plain WPA3 would be a lie told by our own decoder.

### Why there is both `status` and `radio` — and both `mesh` and `mesh nodes`

`radio` prints the brain's cached model, built from unsolicited events. `status` asks the radio
directly. The distinction matters because **events can be lost** — and the SEQ class counter makes
that visible, but visible is not the same as recoverable. Lose one event and the cached model is
wrong permanently, with nothing to correct it.

Every comparable system polls for this reason. Arduino's nina-fw — which has no CRC, no sequence
numbers and no unsolicited channel at all — still cannot lose state, because the host asks.
ESP-AT has `AT+CWSTATE?`. ESP-Hosted has RPC status calls plus a heartbeat. Adding the poll turns
an unrecoverable, invisible loss into a bounded staleness window.

`mesh` and `mesh nodes` are exactly the same pair one layer out, and there the staleness has an
extra source: **a brain that reboots was not listening when the nodes announced themselves.**

`WIFI_STATUS_RESP` also carries the radio's **events-dropped counter**, which previously existed
only in the radio's own debug log — the wrong side of the wire from whoever needs to act on it.

### Scan capacity — a real, documented limit

A scan record is 4 fixed bytes plus up to 32 SSID bytes. With a 1024-byte frame there is room for
**28 networks worst case** (~63 with typical 12-character SSIDs). A dense apartment block can
exceed that, so the radio truncates and says so **on the wire**: the `IOP_FLAG_TRUNCATED` frame
flag is set and a trailing byte gives how many networks were actually *seen*. The count byte always
reports what was actually sent, so the brain never parses past real data, and it can tell "that is
all of them" from "that is the first 28 of 40 in arbitrary order." Without it, a brain ranking
access points by signal strength was silently ranking a subset.

To lift the cap itself: raise `IOP_MAX_FRAME_SIZE` **on both boards**, or chunk the response across
frames using `IOP_FLAG_MORE`. Both doors are open; neither is walked through by default.

---

## Using it

Open the Teensy's Serial Monitor at 115200, line ending **Newline**.

| Command | What it does |
|---|---|
| `help` | list commands |
| `ping` | PING → PONG, with round-trip time in microseconds |
| `scan` | scan for networks, printed as a table with signal bars |
| `connect SSID PASSWORD` | join a network — quote names with spaces: `connect "My Net" pw` |
| `bench [N]` | N round trips; min / mean / max / σ at nanosecond resolution |
| `status` | **ask the radio** what it is doing now — authoritative, and corrects any missed events |
| `radio` | what the brain *believes* about the radio: association, address, reboots, last 802.11 reason |
| `sys` | board, clock, die temperature, link budget |
| `stats` | link health counters |
| `hexdump on\|off` | print every frame in hex, both directions |

Plus the eleven `mesh` verbs in [The control surface](#the-control-surface) above.

### Error-path commands — these are *supposed* to fail

Testing only the happy path tells you only that the happy path works.

| Command | Sends | Expect |
|---|---|---|
| `corrupt` | PING with one bit of the CRC flipped | radio reports bad CRC, returns NACK `0x02` |
| `badcmd` | valid frame, undefined command | NACK `0x01` |
| `oversize` | header claiming a 32767-byte body | NACK `0x05`, then a working link |
| `noise` | junk bytes, then a real PING | PONG — the parser resynchronises |

---

## Verification

### Native test harness

The protocol core has no Arduino dependencies and takes the clock as a function argument, so it
runs and is tested on a PC:

```bash
cl /nologo /O2 /W4 /WX /TP /Fe:host_fuzz.exe host_fuzz.c && host_fuzz.exe
```

```bash
cc -O2 -Wall -Wextra -Werror -o host_fuzz host_fuzz.c && ./host_fuzz
```

Copy `interop_protocol.h` next to `host_fuzz.c` first, or pass `-I ../shared`. It prints its own
total check count and **exits 0 only when every property holds**, so it drops straight into CI.

| Property | Scale |
|---|---|
| Published wire vectors match the implementation | `[DOC]` |
| Round-trip fidelity — hostile payloads (all-`0xAA`), hostile timing, `millis()` rollover | 200,000 random frames |
| Every single-bit error in the CRC-covered span is detected | exhaustive |
| No silent lies under multi-byte burst corruption | 200,000 trials |
| Inter-byte timeout and recovery | `[P2]` |
| NACK storm throttled, suppressions counted | 10,000 → 3 sent |
| SEQ classes never collide | 128 request ids, 128 event ids, no overlap, none skipped |
| A false start with an absurd length does not swallow the next frame *(regression, found on hardware)* | junk absorbed, following PING delivered |
| Precise oversize accounting | `[P7]` |
| Resynchronisation after arbitrary garbage | 20,000 trials |
| Pure noise: no crash, no wedge, no frame delivered whose CRC does not verify | 4,000,000 bytes |

The noise test is where the CRC width shows up as a number rather than an argument: v1's CRC-8
produced 3 accidental frames from 4 MB of noise, and CRC-16 predicts roughly 1 in 4.2 million.

### On-device self-test

All three sketches run their protocol self-test in `setup()` and print the result. It validates the
CRC table against an independent bitwise implementation, checks the published check value
(`0x6F91`), round-trips a frame containing `0xAA`, confirms a flipped bit is caught, confirms
oversize handling and resync, and confirms the builder refuses illegal frames.

If it ever fails, the problem is in the shared header or in how it was copied — **not** in your
wiring. That distinction is the whole reason it exists.

### Three-board capture

```bash
python tools/capture_baseline.py 25 baseline.log
```

Debugging a three-board system by watching three serial monitors does not work. Each board has its
own oscillator and its own boot time, so "the node says up 285 s" and "the hub says up 291 s"
cannot be compared — they are not measuring from the same zero. Worse, you cannot tell whether the
hub logged something *before* or *after* the brain asked for it, which is usually the entire
question.

`capture_baseline.py` opens every board at once and timestamps every line against a single clock
(the PC's). That one change turns three unrelated logs into one causal narrative:

```
[  6.940] BRAIN | >>> status
[  6.956] HUB   | [cmd] WIFI_STATUS_REQ seq=2 -> state=idle
[  6.965] BRAIN | > [status] idle
```

16 ms request-to-log on the hub, 9 ms back. You cannot see that in three windows. It is the same
principle `mesh_protocol.h` applies on the air: never compare two free-running clocks — pick one
and put everything on it.

The second reason it exists is a baseline you can **diff**. Run it before and after any significant
change. Without a recorded starting point, a regression looks exactly like a feature that never
worked, and the difference decides whether you debug the new code or the old. The first capture
paid for itself immediately: it found three real defects, written up in
[`docs/PLATFORM_BASELINE.md`](docs/PLATFORM_BASELINE.md).

---

## Milestone checklist

**1 — Handshake.** Upload both. Teensy prints `LINK ESTABLISHED`.
`ping` → PONG with a microsecond RTT. `corrupt` → NACK `0x02`. `badcmd` → NACK `0x01`.
`oversize` → NACK `0x05`, then `ping` still works. Unplug the Teensy TX wire, `ping` → timeout
after 500 ms, no hang. Reconnect, `ping` works again.

**2 — Scan.** `scan` → table of networks with RSSI, channel, encryption. Negative RSSI values
(if they print as ~189 instead of ~−67, a signed cast is missing). Hidden networks show
`<hidden>`. With no networks in range, `0 networks found` — reported as a result, not an error.

**3 — Connect.** `connect SSID PASSWORD` → `CONNECTED` plus an IP. Wrong password → status `0x01`
with the raw 802.11 reason. Nonexistent SSID → status `0x02`. `radio` reflects the state
throughout.

**5 — Fleet.** Power the node. The hub reports it and the brain prints a node-seen event without
being asked. `mesh nodes` → the roster with MACs. `mesh id 0` → firmware version and
`WHO_AM_I = 0x70`. `mesh ping 0` → a hub-timed round trip in the low milliseconds, 0% loss.
`mesh watch on` → live samples with `az ≈ 17340` counts when the board is flat. `mesh mode 0 idle`
→ the **node's own console** prints `[cfg] mode=2`, and about five seconds later both the hub and
the brain declare it LOST. `mesh mode all interrupt 200` → one broadcast packet brings it back.
`mesh stream off` → `mesh status` shows `rx` still climbing while `relayed` is frozen.

---

## Troubleshooting

**Start here:** type `stats` on the Teensy and read **bytes discarded**. `0` with no valid frames
means nothing is arriving electrically (no wire, wrong pins, TX→TX). A steadily climbing count with
no valid frames means a baud mismatch. That one number separates electrical faults from protocol
faults immediately.

| Symptom | Most likely cause |
|---|---|
| Nothing at all from an ESP32's USB port | `Serial` is going to the *other* connector — flash with `CDCOnBoot=cdc` |
| Link latency suddenly 5× worse, huge σ | Same cause. A blocking UART0 console, not the mesh — see [Building](#building) |
| `LINK FAILED`, no PONG | TX↔RX not crossed; or GND not shared; or baud mismatch |
| Steady discarded bytes, zero valid frames | Baud rate mismatch — the classic signature |
| A few bad CRCs per thousand frames | Grounding, wire length, or missing bulk capacitance |
| Bad CRC on *every* frame | The boards are running different `interop_protocol.h` — run `sync_headers.py`, re-upload all of them |
| "Protocol version mismatch" at boot | One board is v1 and one is v2. They cannot interoperate; re-flash both |
| PING fine, scans "randomly" fail | RX buffer too small on the receiving side |
| Radio reports `BROWNOUT` | The 3.3 V rail sags when the transmitter keys up — power problem, not protocol |
| Teensy shows as "unknown" with no COM port | It is in the HalfKay bootloader (USB PID `0478`). Tap its button, or flash it with `tools/teensy_halfkay_flash.py` |
| `Unable find Teensy Loader` on upload | Loader IPC, not your board. Use the HalfKay flasher — see [Flashing](#flashing) |
| OLED blank, but I²C scan finds `0x3C` | Wrong driver. 0.96" needs SSD1306, 1.3" needs SH1106 |
| OLED shows nothing at all | Check VCC is **3.3 V**, and that SDA=18 / SCL=19 are not swapped |
| Console output stops during a scan | Working as intended — lines are dropped rather than blocking the link. Count is in `stats` |
| WiFi "randomly drops" | Check `radio` — if announcements > 1, the radio is rebooting on its own |
| **Mesh initialises fine, every send returns OK, nothing is ever received** | **The two radios are on different channels.** This is the single most common way an ESP-NOW mesh fails silently. `mesh init 1` on the hub; the node defaults to channel 1 |
| `mesh nodes` is empty after a Teensy reset | Expected. Node-seen events only reach a brain that was up. `mesh nodes` polls — that is what it is for |
| Node reports `[imu] NOT FOUND` | Check SDA=GPIO8, SCL=GPIO9, VCC=3.3 V, GND shared with the node |
| `WHO_AM_I` is not `0x68` | Also expected here — `0x70` is an MPU-6500-family part. The register map is compatible |
| Samples arrive but read `a = 0 0 0` with a 1 µs latency | A decoder offset bug, not a sensor fault. The wire offsets are derived in `mesh_protocol.h`; do not hand-count them |
| Node goes LOST for no reason | It was told to. Check whether something sent `mesh mode … idle` — an idle node is reachable but silent, and silence past 5000 ms is LOST by definition |

---

## Prior art — what this is and is not

This architecture is **not novel**, and it is worth saying so plainly so nobody builds on a claim
that will not survive contact with a search engine.

| Project | What it is |
|---|---|
| [ESP-Hosted-MCU](https://github.com/espressif/esp-hosted-mcu) | Espressif's own: any host MCU + ESP as radio, over SPI/SDIO/UART. 12-byte binary header with a 16-bit checksum **and** a 16-bit sequence number |
| [Teensquitto](https://github.com/tonton81/Teensquitto) | Teensy + ESP8266 WiFi coprocessor, 2017. Custom binary framing, XOR checksum, no sequence number |
| [esp-link](https://github.com/jeelabs/esp-link) / [espduino](https://github.com/tuanpmt/espduino) | ESP as coprocessor with SLIP framing and CRC-16, ~2015. Recommended on the PJRC forum for Teensy+ESP links |
| [WiFiNINA_Generic](https://github.com/khoih-prog/WiFiNINA_Generic) | ESP32 as radio over SPI, supports Teensy 4.1. **What Teensy users actually run today.** No CRC, no sequence numbers |
| [SerialTransfer](https://github.com/PowerBroker2/SerialTransfer) | Sentinel / ID / length / payload / CRC-8 / terminator — nearly this frame shape, used for Teensy↔ESP32 |
| [tinyproto](https://github.com/lexus2k/tinyproto) | HDLC-style link layer with CRC and real sliding-window sequencing, runs on both Teensy and ESP32 |
| [Klipper](https://www.klipper3d.org/Protocol.html) | `length · sequence · content · CRC-16 · sync`, runs on Teensy, in 3D printing, on hundreds of thousands of machines |

So: the pattern is a product category, the framing is well-trodden, and "binary framing with a CRC
and sequence numbers on a Teensy over UART" is already shipping at scale in this very domain.

**What is honestly distinctive** is narrower. There is no public Teensy binding of the vendor's
own coprocessor design, and the closest Teensy-specific work uses an XOR checksum with no
sequence numbers, no written spec and no tests. Against that baseline, this project's
resynchronising parser, verified CRC, native assertion harness and documented failure log are a
real step up — *better-built than the existing Teensy option*, not first. The fleet layer narrows
it further: a Teensy that configures remote ESP-NOW nodes through a UART-attached hub, with the
subscription and the loss statistics kept independent, is not something you can go and download.

One claim here **is** specific and checkable: this framing is more robust than Espressif's own
UART transport. ESP-Hosted has no start marker, no byte stuffing and no resync — both ends read a
fixed 12 bytes and `continue` on error from the same misaligned offset, so one stray byte
desyncs it permanently ([#51](https://github.com/espressif/esp-hosted-mcu/issues/51),
[#142](https://github.com/espressif/esp-hosted-mcu/issues/142)). Their fix was to flush the RX
FIFO after reset rather than add a sync marker.

## What this protocol deliberately does not do

Documented so the next person knows which walls are load-bearing and which are just walls.

- **No byte stuffing / COBS.** Framing relies on LEN + CRC rather than an escaped delimiter.
  Simpler and denser; the trade is the 1-in-4.2-million analysis above. If you need provably
  unambiguous framing, COBS is the standard answer — but it changes the wire format, so both
  sides must change together.
- **No ACK of successful frames, no automatic retry.** A NACK tells you something broke; the
  policy of what to do about it lives on the brain, on purpose. Automatic retries hide problems
  during bring-up, which is exactly when you need to see them. The same holds on the air: ESP-NOW
  loses packets, and the node's sequence number makes that **counted rather than papered over**.
- **No flow control** — though both boards have the hardware. The Teensy drives RTS from any
  digital pin and CTS through the XBAR (pins 2,3,4,5,7,8,30,31,33,36,37,42,43,44,45); the ESP32
  has `setHwFlowCtrlMode()`. It is off because request/response traffic never saturates the link
  and the receive buffer holds eight max-size frames. Set `LINK_USE_FLOW_CONTROL` to 1 when you
  add sustained streaming.
- **No framing on the ESP-NOW side.** Not an omission — see [Two protocols, deliberately
  different](#two-protocols-deliberately-different). The transport already provides boundaries, a
  CRC and addressing.
- **No scaling of sensor data anywhere below the brain.** Raw counts cross two links untouched.
- **One outstanding request at a time.** SEQ is already on the wire, so pipelining is a change to
  the endpoints, not to the format. The ESP32 executes one long-running WiFi operation at a time
  and answers a second request with a BUSY NACK; modelling that honestly with one slot and a clear
  refusal is more debuggable than a queue that hides the constraint.
- **No BLE yet.** Channel `0x02` is reserved, commands `0x20`–`0x28` are numbered and `IOP_CAP_BLE`
  is defined, so a BLE-capable radio is a drop-in rather than a protocol revision.
- **No OTA.** Nothing reserved, nothing started.

---

## Extension points, in rough order of usefulness

| Want | Do this |
|---|---|
| More nodes | `MESH_MAX_NODES` is 8 on the hub. The roster and the events scale with it |
| A different kind of node | `mesh send <t> <hex…>` talks to it before you have written a single line of brain-side code |
| More than 28 networks per scan | Raise `IOP_MAX_FRAME_SIZE` on both boards, or chunk with `IOP_FLAG_MORE` |
| Several radios | The Teensy has 8 hardware UARTs; change `LINK_PORT` |
| A PC-side decoder | Build the Teensy with USB Type **Dual Serial**; raw frames stream to `SerialUSB1` |
| Frame logging to disk | Teensy 4.1 has a built-in microSD slot |
| Higher throughput | Both UARTs go well past 921600 on short traces; raise `LINK_BAUD` on both, then run `bench` |
| BLE | Channel `0x02`, commands `0x20`–`0x28`, set `IOP_CAP_BLE` in the radio's `RADIO_READY` |
| Bridge to a network | Teensy 4.1 has 10/100 Ethernet PHY pads |

---

## File map

| File | What it is |
|---|---|
| `shared/interop_protocol.h` | The UART protocol. CRC-16, parser, builder, self-test, all constants. Zero Arduino dependencies — that is what makes it portable and testable. |
| `shared/mesh_protocol.h` | The ESP-NOW protocol. Packed structs, message types, sampling modes, derived wire offsets. Also zero dependencies. |
| `esp32_wireless_bridge/…ino` | The hub. Async scan and connect state machines, unsolicited events, the ESP-NOW roster, liveness, and the relay. |
| `esp32_sensor_node/…ino` | A node. IMU on I²C, data-ready ISR that only timestamps, ESP-NOW reporting. Decides nothing. |
| `teensy_master_controller/…ino` | The brain. Console, radio and fleet state models, latency instrumentation, sample decode. |
| `tools/sync_headers.py` | Keeps both headers identical across all three sketch folders. |
| `tools/host_fuzz.c` | Native test harness for the UART protocol core. |
| `tools/capture_baseline.py` | Opens all three boards at once and timestamps every line on one clock. |
| `tools/teensy_halfkay_flash.py` | Flashes a Teensy 4.x directly over HalfKay/HID. No Loader GUI, no localhost IPC — written because that path failed permanently on the build machine. |
| `docs/MESH_ARCHITECTURE.md` | The fleet layer end to end: packet layouts, discovery and liveness, the broadcast path, and the reasoning behind each. |
| `docs/PLATFORM_BASELINE.md` | The timestamped three-board hardware capture, and the three defects it found. |
| `docs/BRINGUP_LOG.md` | Every failure encountered building this, and what each one taught. The most useful document here if something is not working. |
| `docs/protocol_reference.html` | The visual protocol reference, with a live frame decoder. |
