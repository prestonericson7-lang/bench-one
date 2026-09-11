# Platform baseline — 2026-09-03 23:55

**What this document is:** a timestamped, measured statement of what the three-board platform
actually does, captured before adding anything else to it.

**Why it exists:** every number below was taken from hardware in a single 25-second window with
all three boards running and one clock (the PC's) timestamping all of them. Without a record like
this, a regression six commits from now is indistinguishable from a feature that never worked —
and worse, we could spend a week building on top of a defect we never noticed.

That is not hypothetical. **This capture found a real design defect** that had been running
silently and would have poisoned everything built on top of it. It is written up in
[Defect 1](#defect-1--the-radio-decides-what-the-brain-hears) below. The baseline paid for itself
the first time it ran.

---

## The platform

```
  ┌──────────────┐   UART 921600 8N1    ┌──────────────┐   ESP-NOW ch 1   ┌──────────────┐
  │  Teensy 4.1  │  interop_protocol.h  │  ESP32-S3 #1 │ mesh_protocol.h  │  ESP32-S3 #2 │
  │    BRAIN     │◄────────────────────►│     HUB      │◄────────────────►│     NODE     │
  │              │  pin1→GP18, GP17→pin0│   (radio)    │                  │  + MPU-6500  │
  └──────────────┘                      └──────────────┘                  └──────────────┘
       COM38                                 COM27                             COM29
    600 MHz M7                        28:84:85:54:E1:AC                 1C:DB:D4:46:D5:0C
```

| Role | Board | Port | Identity |
|---|---|---|---|
| Brain | Teensy 4.1 (IMXRT1062) | COM38 | VID 16C0 / PID 0483 |
| Hub / radio | ESP32-S3 | COM27 | MAC `28:84:85:54:E1:AC` |
| Sensor node | ESP32-S3 + GY-521 | COM29 | MAC `1C:DB:D4:46:D5:0C`, `WHO_AM_I = 0x70` |

**The IMU is not an MPU-6050.** `WHO_AM_I` reads `0x70`; a genuine MPU-6050 reads `0x68`. `0x70`
is an MPU-6500-family part or clone. The register map used here is compatible, but the sensor
identifies itself honestly in every `MESH_MSG_HELLO`, so the brain always knows what it is
actually talking to rather than what the eBay listing said. **Do not assume a board marked
"GY-521" contains the chip it claims** — read `WHO_AM_I` and report it upward.

### Firmware and protocol versions

| Item | Value |
|---|---|
| UART protocol | **v2.0**, CRC-16/MCRF4XX, check value `0x6F91` |
| Max frame | 1024 bytes (1015 payload) |
| Mesh protocol | fw 1.0 |
| Parser state | 1068 bytes |
| Teensy RX buffer | 8192 bytes = **88.9 ms** of headroom at 921600 |
| DWT resolution | 1666 ps/tick @ 600 MHz |
| Toolchains | `teensy:avr 1.62.0`, `esp32:esp32 3.0.7` |

Header copies verified identical to master at capture time:

```
shared/interop_protocol.h   sha256 6a6a3dd775291528   [ok] x2
shared/mesh_protocol.h      sha256 d7ecada105394597   [ok] x2
```

---

## What works — measured, not assumed

### The full chain carries every sample, end to end, with zero loss

This is the headline result. One number is produced at each of four stages by four independent
counters on three boards across two different transports:

| Stage | Counter | Rate | Loss |
|---|---|---|---|
| IMU data-ready interrupt | node `irq` | **201.96 Hz** | — |
| ESP-NOW transmit | node `sent` | **202.0 Hz** | `fail = 0` |
| ESP-NOW receive | hub `rx` | **201.9 Hz** | `dropped = 0`, node `lost = 0` |
| UART frames parsed | brain `valid frames` | **201.6 Hz** | `discarded = 0` |

All four agree within **0.2%**. Every sample the sensor produced reached the brain.

Derivation, so these are checkable rather than asserted:

```
node   t=4.757  irq=53043 sent=53033   →   t=24.716  irq=57074 sent=57065
       Δ 19.959 s:  irq +4031 = 201.96 Hz     sent +4032 = 202.0 Hz     fail = 0 throughout

hub    t=5.391  rx=53170              →   t=15.378  rx=55186
       Δ  9.987 s:  rx +2016 = 201.9 Hz        dropped = 0

brain  t=5.440  valid=51614           →   t=13.943  valid=53530
       Δ  8.503 s:  +1916 frames, of which 202 were bench PONGs
                    → 1714 mesh frames = 201.6 Hz      discarded = 0
```

The IMU is configured for 200 Hz and free-runs at 201.96 Hz — a **0.98% fast** internal
oscillator, which is entirely normal for an uncompensated MEMS part and is exactly why the node
timestamps its own samples rather than letting the hub assume a rate.

### The UART link is clean

Brain-side parser counters after 53,530 frames:

```
valid frames ........... 53530
bad CRC ................ 0
impossible length ...... 0
oversize ............... 0
frame timeouts ......... 0
bytes discarded ........ 0
NACKs suppressed ....... 0
console lines dropped .. 0
radio events missed .... 0
```

Zero errors of every class, at 921600 baud, over jumper wire. `bytes discarded = 0` alongside
53,530 valid frames is the strongest possible statement that the physical layer is sound — the
receiver never saw a single byte it could not place inside a frame.

Hub-side: `ok=205 badcrc=0 discarded=391`. **The 391 is stale, not ongoing** — it read 391 at
t=5.391 and still read 391 at t=15.378, ten seconds and 200 frames later. It was accumulated once
at boot, when the Teensy's own USB-serial banner or a reset transient hit the line before either
side was synchronised. A frozen counter is a historical event; a climbing one is a live fault. It
is worth checking twice rather than once for exactly this reason.

### Command/response works

```
requests sent .......... 203
responses received ..... 202     (the 203rd was still in flight when stats was printed)
NACKs received ......... 0
timeouts ............... 0
stale/unsolicited ...... 0
```

`ping` → `PONG seq=3, round trip 573.253 us`. `status` → radio answered `idle`. `bench` completed
200/200 with zero lost.

### The wiring, confirmed

The intermittent jumper that blocked this work is fixed. Both directions carry traffic:
brain→hub (203 requests, all answered) and hub→brain (53,530 frames, zero errors).

---

## Defects this baseline found

### Defect 1 — the radio decides what the brain hears

**This is the important one, and it is a design error, not a bug.**

The hub relays *every* ESP-NOW packet to the brain the instant it arrives, unconditionally, with
no way to turn it off. The brain never asked for the stream. It cannot decline it. It cannot
sample it. It gets all 202 packets per second forever, starting 8 seconds after the hub boots.

That is **policy on the radio**, and it is a direct violation of the rule this whole project is
organised around:

> Mechanism lives on the radio. Policy lives on the brain.

"Should this telemetry go to the brain, and how much of it" is a decision about what the
application needs. It is the brain's call. The radio's job is to *be able* to relay, and to relay
when told to.

The relay code even carries a comment defending the right half of the decision:

```c
/* Relay VERBATIM to the brain, prefixed with the source MAC. The hub does not parse,
 * rescale or summarise the node's payload -- it is a relay, and interpreting sensor
 * data here would put policy in the wrong place. */
```

Not summarising the payload is correct. But *choosing to send it at all* is the same category of
decision, and that half was never questioned. **It is easy to get the visible half of a principle
right and miss the invisible half.** Nobody decided the relay should be unconditional; it simply
never occurred to anyone that it was a decision.

**Three measured consequences:**

**1. Round-trip latency got worse and far less predictable.**

> ## CORRECTION — this claim was wrong, and the correction is the useful part
>
> The section below originally blamed the unconditional relay for a latency regression, with a
> table showing the worst case tripling. **That diagnosis was false.** It is left here, struck
> through in substance, because how it was caught matters more than the original claim.
>
> Once the relay became switchable, the obvious experiment took one sitting: run the same
> 300-ping benchmark at three subscription settings on the same boards, minutes apart.
>
> | Stream setting | min | mean | max | σ |
> |---|---|---|---|---|
> | off — relaying nothing | 412.99 µs | 2248.98 µs | 10752.05 µs | 3899.88 µs |
> | all — 202 frames/s | 413.31 µs | 2241.67 µs | 11177.01 µs | 3690.16 µs |
> | 1-in-20 | 412.93 µs | 2259.34 µs | 10796.37 µs | 3894.19 µs |
>
> **Identical.** Turning the firehose completely off changed nothing. The relay was never the
> cause.
>
> **The actual cause was the debug console.** The ESP32 sketches had been flashed with an
> `arduino-cli` FQBN that omitted `CDCOnBoot=cdc`. That option defaults to *Disabled*, which
> makes `Serial` map to UART0 on GPIO43/44 instead of the native USB. The hub prints one line per
> handled command, and those lines were now going to a 115200-baud hardware UART with nothing
> draining it, so `write()` blocked. The arithmetic confirms it: 300 pings × ~30 bytes × 10 bits
> at 115200 baud is **0.78 s** of pure UART time, and the benchmark spent about **0.68 s**.
>
> With the console back on USB CDC and nothing else changed:
>
> | | mean | max | σ |
> |---|---|---|---|
> | Original baseline (mesh relaying, CDC console) | 500.09 µs | 1001.44 µs | 133.46 µs |
> | Broken console (UART0, blocking) | 2261.35 µs | 10782.27 µs | 3894.08 µs |
> | **Corrected (CDC console, 300/300, 0 lost)** | **435.94 µs** | **635.40 µs** | **42.39 µs** |
>
> The link is now better than the original baseline on every statistic.
>
> **Three lessons, in order of how much they cost:**
>
> 1. **A plausible mechanism plus a real symptom is not evidence of causation.** The relay was
>    genuinely doing work, the latency was genuinely bad, and the story connecting them was
>    genuinely wrong. Nothing about the reasoning felt weak at the time — which is the point.
> 2. **Your instrumentation is part of the system under test.** The measurement was distorted by
>    the thing printing the measurement. This is the same trap as the cascade assertion in
>    section 3, which passed *because* of the bug it was meant to catch.
> 3. **Build the switch, then measure.** The A/B was only cheap because the subscription had been
>    built. That work was worth doing anyway — the unconditional relay *was* a real design error,
>    for the reasons in Defect 1 below — but its first real payoff was making a wrong hypothesis
>    falsifiable in one sitting instead of requiring a reflash.
>
> The original, incorrect text follows for the record.

~~The figures below were attributed to the relay. They are real measurements of a real
regression, but of the console problem described above, not of the relay.~~

| | Before the mesh existed | With the mesh relaying | Change |
|---|---|---|---|
| min | ~250 µs | 427.6 µs | +71% |
| mean | — | 500.1 µs | — |
| max | 299 µs (701 requests) | 1001.4 µs | 3.3× worse |
| std dev | tight | 133.5 µs | — |

> The original caveat below turned out to be pointing at the right doubt for the wrong reason —
> the comparison was indeed untrustworthy, but because of an uncontrolled variable nobody had
> thought of, not because of the sample sizes.

> **Caveat, stated honestly:** the "before" figures are from an earlier session under the same
> command (`bench`, 200+ round trips, same baud, same boards) but not from a controlled A/B in
> one sitting. They are strong evidence, not proof. A clean A/B is trivial once the fix below
> lands — turn the stream off and re-run `bench` — and that measurement is the first thing to do
> after the fix. Quoting an old number next to a new one and calling it a result is exactly the
> kind of thing this project is supposed to be better than.

**2. The brain's console became unusable.** Of 5,071 lines the Teensy emitted in 25 seconds,
**4,957 — 97.8% — were this one line:**

```
[radio] unsolicited UNKNOWN_CHAN/UNKNOWN_CMD that this brain does not understand -- ignored
```

roughly 198 times per second. Real output — `sys`, `stats`, `bench` results — was buried in it.

Note what the brain did *right* here: it received a channel it had no handler for and **ignored
it and said so**, rather than misparsing it or crashing. Forward compatibility worked exactly as
designed. It was simply too polite about it, 198 times a second.

**3. The relay competes with command handling in the same loop.** The hub drains exactly one
queued packet per `loop()` pass. At 202 packets/s that keeps up — but it means every PING waits
behind whatever mesh traffic is already queued for transmission, which is the likely source of
the latency spread above.

**Not the cause:** bandwidth. Each relayed frame is 39 bytes (6 MAC + 24 mesh + 9 framing), so
202/s is 78.6 kbit/s of a 921.6 kbit/s link — **8.5% utilisation**. There is plenty of wire. The
cost is loop time and attention, not throughput. Worth stating because "it's saturating the link"
is the obvious guess and it is wrong.

**The fix: subscription. The stream is off until the brain asks for it, and the brain says how
much it wants.** Tracked as the immediate next change.

### Defect 2 — the brain has no MESH handlers

`interop_protocol.h` defines the full MESH command set (`0x30`–`0x3B`) and the hub implements
every one of them. The Teensy implements none, which is why it can only report
`UNKNOWN_CHAN/UNKNOWN_CMD`. The fleet currently runs because the hub **autostarts after 8 s** when
the brain stays silent — a deliberate standalone fallback, but right now it is the *only* thing
keeping the mesh alive, which means an intended fallback is doing load-bearing work.

### Defect 3 — ESP-NOW air latency is still unmeasured

The hub has a complete hub-clock-timed ping train (`MESH_PING_REQ` → `MESH_PING_RESP` with
min/mean/max). It has never run, because only the brain can trigger it and the brain has no MESH
commands. The one number the second board was added to produce does not exist yet.

---

## Honest scorecard

| Capability | State | Evidence |
|---|---|---|
| UART framing v2, both directions | **Working** | 53,530 frames, 0 errors of any class |
| Command/response + NACK paths | **Working** | 203 requests, 202 responses, 0 timeouts |
| DWT nanosecond latency | **Working** | 200/200 bench, 1666 ps resolution |
| WiFi scan / connect | **Working** | milestones 1–3, verified earlier |
| Radio status poll | **Working** | `status` → `idle` |
| ESP-NOW hub + node discovery | **Working** | node found, peer added, liveness tracked |
| IMU sampling at 200 Hz | **Working** | 201.96 Hz, `fail = 0` |
| End-to-end telemetry, zero loss | **Working** | four counters agree within 0.2% |
| Brain-directed mesh control | **Missing** | Defect 2 |
| Relay subscription control | **Broken by design** | Defect 1 |
| ESP-NOW air latency figure | **Missing** | Defect 3 |
| BLE | **Not started** | reserved `0x2x` |
| OTA | **Not started** | — |

**Verdict: the transport layers are sound and measured. The control layer above them is
incomplete, and one piece of it is actively wrong.**

Defect 1 is worth fixing on its own terms — an unconditional push puts policy on the radio, and
every future feature that relays data would inherit it. But note, after the correction above,
that it was **not** the performance problem it was originally blamed for. Fixing it was right;
the stated justification was wrong. Both halves of that sentence are worth keeping.

---

## Reproducing this capture

```bash
python tools/capture_baseline.py 25 baseline.log
```

Opens all three boards simultaneously, drives the Teensy console through
`help / sys / stats / status / radio / ping / bench / stats`, and timestamps every line from every
board against one clock. Re-run it after any significant change and diff the scorecard.

**Why one script and not three terminals:** the boards' own uptime counters have no relationship
to each other — three independent oscillators, three different boot times. Interleaving all three
onto the PC's clock is what makes "the hub logged this 0.9 s after the brain asked for it"
a statement you can actually make. The same reasoning that keeps
[mesh_protocol.h](../shared/mesh_protocol.h) from ever comparing two boards' timestamps applies to
the log file: pick one clock and put everything on it.
