# Bring-up log: everything that went wrong, and what it taught

This is the honest record of building the ESP32-S3 ↔ Teensy 4.1 link — the wrong turns included.
It exists because the failures turned out to be more valuable than the successes, and because
every one of them is reproducible by the next person.

Most embedded write-ups show you the working result. This one shows the fourteen things that
didn't work first, because that's the part you'll actually need.

---

## The single most useful diagnostic in this whole project

When the link is dead, one number tells you which class of fault you have. Type `stats` on the
Teensy and look at **bytes discarded**:

| `bytes discarded` | `valid frames` | What it means |
|---|---|---|
| **0** | 0 | **Nothing is arriving at all.** No wire, wrong pins, or TX↔TX. Electrical. |
| climbing steadily | 0 | Bytes arrive but never parse. **Baud mismatch** — the classic signature. |
| occasional | climbing | Healthy link with some noise. Check grounding and wire length. |
| 0 | climbing | Perfect. |

The distinction between the first two rows saves hours. "Zero discarded" is not "no data" — it's
positive evidence that the receiver's UART never saw a single edge, which rules out every
software cause immediately.

That's why the parser counts discarded bytes at all. It was added as an afterthought and turned
out to be the highest-value line in the debug output.

---

## 1. The wiring was straight-through, not crossed

**Symptom:** PING timed out. ESP32 printed nothing. `discarded = 0` on both sides.

**Cause:** ESP TX → Teensy TX1, ESP RX → Teensy RX1. Both transmit outputs were tied to each
other, and both receive inputs were tied to each other with nothing driving them.

**Why it's so confusing:** nothing is electrically wrong. There's no smoke, no error, no
brownout. Both boards are healthy and both are talking — into their own ear. A UART is a
crossover connection: **TX goes to RX.**

```
WRONG                                  RIGHT
ESP TX ──── Teensy TX1                 Teensy pin 1 (TX1) ──→ ESP32 GPIO18 (RX)
ESP RX ──── Teensy RX1                 Teensy pin 0 (RX1) ←── ESP32 GPIO17 (TX)
```

**Aside worth knowing:** while TX was tied to TX, one board drove the net low during transmission
while the other held it high — brief output contention. At 3.3 V over a short jumper this is
survivable, but it is not something to leave running.

**Also:** don't wire to the pins silkscreened `TX`/`RX` on an ESP32-S3 devkit. Those are usually
GPIO43/44 — UART0 — which carries the ROM bootloader banner at boot and is shared with the
onboard USB-serial bridge. Use numbered GPIOs clear of the reserved ranges.

---

## 2. Two firmware bugs that only real hardware found

Both survived 1.4 million assertions in a native fuzzer. Both died in the first ten minutes on
actual boards. This is the section to read if you think a good test suite means you can skip
bring-up.

### Bug A — a false start swallowed a real frame

The `noise` test injects junk containing a stray `0xAA`, then sends a valid PING. The PING
vanished.

The junk `AA AA 12 34 …` was parsed as a frame header declaring **`LEN = 0xAA12` = 43,538
bytes**. The parser dutifully entered its SKIP state to drain that many bytes — and ate the
entire PING that followed, plus everything else, until the 50 ms timeout rescued it.

The SKIP state exists for a good reason: when a peer sends a frame too big for our buffer,
draining it precisely avoids resyncing on a byte *inside* its payload. But it assumed the
declared bytes were actually coming. For a false start, nothing is coming.

**The fix — separate "too big for us" from "not a real frame":**

```
len + 1 ≤ IOP_MAX_SKIP_BYTES  →  believable frame from a peer with a bigger buffer. Drain it.
len + 1 > IOP_MAX_SKIP_BYTES  →  no implementation can emit that. Report and resync NOW.
```

The ceiling is 2× our frame size: tolerant of a peer with double our buffer, while capping the
blast radius of a false start at 2048 bytes (22 ms) instead of 4,600 bytes.

The README had claimed "a false start costs at most one frame." That was simply wrong, and
hardware proved it.

### Bug B — an oversize frame reported the wrong reason

The `oversize` test sends a header claiming a 32767-byte body and then stops. Expected NACK
`0x05` (payload too large); got a generic timeout and no NACK at all.

SKIP could never drain — the promised bytes were never sent — so the 50 ms tick fired first and
discarded the *diagnosis* along with the frame. The peer needs "payload too large" to send the
right NACK; "something timed out" tells it nothing.

**The fix:** when a timeout fires while in SKIP, report the stored reason (with the captured
SEQ) instead of a bare timeout. The frame is equally dead either way; the difference is whether
the log says anything useful.

---

## 3. Two tests that passed for the wrong reason

More alarming than the bugs.

### The cascade assertion encoded the bug it was meant to catch

The noise test asserted `oversize_events < nbytes / 100` and passed comfortably — 110 events in
4 MB. After fixing Bug A it **failed**, at 52,843 events.

The original number was low because false starts were entering SKIP and silently swallowing
thousands of bytes each. **The passing test was measuring the bug.** A threshold tuned against
observed output will happily certify whatever behaviour produced that output.

Rewritten to assert the invariant that actually matters — *one false sync produces at most one
error event* — checked against the count of `0xAA` bytes actually fed, not against a fraction of
the stream.

An accidental benefit: with false starts now reported rather than hidden, the numbers became
measurable. 3 accidental frames from 52,843 candidates is **1 in 17,614**, against the
1-in-16,000 the design predicted from first principles. The theory was right.

### The corruption test corrupted itself

The burst-corruption test reported "13 of 200,000 corrupted frames delivered as if intact" —
which reads as the CRC missing 13 errors.

It wasn't. The test applied 1–3 random XOR masks at random positions; occasionally two landed on
the same byte with the same mask and **cancelled out**. The frame arrived genuinely intact and
was counted as a CRC failure.

Fixed by snapshotting the frame and skipping trials where corruption cancelled. The number went
to **0 of 199,987**. A test that overstates a failure is worse than no test — it teaches you to
ignore its output.

---

## 4. The toolchain fought back: Teensy Loader IPC

The longest detour, and the one most likely to hit the next person.

**Symptom** — `arduino-cli` upload to Teensy 4.1 on Windows:

```
Opening Teensy Loader...
Unable find Teensy Loader.  (p)  Is the Teensy Loader application running?
Is a firewall (eg, ZoneAlarm) blocking localhost communication?
```

**Every clause of that message is a red herring.** What was measured:

| Hypothesis from the error | Actual finding |
|---|---|
| Loader not running | Running, PID confirmed, correct path |
| Loader hung | `Responding = True`, valid window handle, wxSocket window present, no modal dialog |
| Firewall blocking localhost | No firewall rule references any teensy executable; a .NET `TcpClient` connected to its listener on `127.0.0.1:3149` fine |
| Port conflict | `COM38` opened cleanly; nothing holding it |

**Everything tried:**

| Attempt | Result |
|---|---|
| `--upload -p usb:0/…` with board in RawHID | **worked, once** |
| Same, after the board ran a USB-Serial sketch | `(p)` failure |
| Retry with loader already up | same |
| Address the board by `-p COM38` | same |
| Kill + restart the loader | same |
| Let `teensy_post_compile` spawn its own loader | it spawns one, then can't reach it |
| `teensy_reboot.exe` standalone | same failure, code `(r)` |
| Board already sitting in HalfKay | same |

**Resolution: stop fighting it.** The IPC layer is the fragile part, so delete it.

```
before:  arduino-cli → teensy_post_compile → [localhost IPC] → teensy.exe GUI → board
after:   tools/teensy_halfkay_flash.py → [Windows HID] → board
```

`tools/teensy_halfkay_flash.py` speaks HalfKay directly. No GUI, no localhost, nothing to click.

### Two things that made the flasher possible

**The 134-baud reboot trick.** Opening a Teensy's USB serial port at 134 baud makes a running
sketch jump to the HalfKay bootloader. No button, no loader, no IPC. This is how the board gets
into a programmable state from software. Confirmed working: COM38 vanished on cue.

**The address base, deduced not guessed.** A Teensy 4.x hex file is based at `0x60000000`. But
HalfKay's address field is **three bytes** — 24 bits, a 16 MB ceiling. `0x60000000` cannot fit,
so wire addresses must be flash-relative and the base must be subtracted. That conclusion comes
from the *width of the field*, not from documentation, which is why it can be trusted.

Protocol constants were read from PJRC's actual `teensy_loader_cli.c`, not recalled. Windows then
independently confirmed the packet size: `OutputReportByteLength = 1089` = 1088 payload + 1 report
ID, exactly as computed.

### A ctypes bug worth calling out

First run died with `OverflowError: int too long to convert`. I had declared `restype` but not
`argtypes`, so ctypes defaulted arguments to C `int` — 32 bits — and a 64-bit `HDEVINFO` handle
wouldn't fit.

That error was loud. **The same mistake on a different value silently truncates a pointer and
hands the kernel garbage.** Always declare `argtypes` for Win32 calls, not just `restype`.

---

## 5. Bootloader ergonomics: the two boards are opposites

Confusing these costs an afternoon.

| | ESP32-S3 | Teensy 4.1 |
|---|---|---|
| Enter bootloader | **Hold** BOOT, tap RESET, keep holding | **Single short tap** of the button — holding does nothing |
| From software | auto (esptool toggles DTR/RTS) | open its port at **134 baud** |
| Programmed via | serial, `esptool` — fully scriptable | HID, HalfKay — normally via the Loader GUI |
| Speed | ~6 s for 890 KB | well under a second for 87 KB |
| Identify state | — | USB PID: `0478` bootloader, `0483` USB-Serial sketch, `0486` RawHID |

That PID table is the fastest way to know what a Teensy is doing. If the IDE shows it as
"unknown" with no COM port, it's `0478` — sitting in the bootloader, waiting.

---

## 6. `Serial` on the ESP32-S3 doesn't go where you think

On the ESP32-S3, `Serial` is a **macro**, and what it resolves to depends on a board menu:

| Tools → USB CDC On Boot | `Serial` becomes | Output appears on |
|---|---|---|
| **Disabled** (the default) | `Serial0` = UART0 on GPIO43/44 | the port labelled **UART** |
| Enabled | `HWCDCSerial` = native USB | the port labelled **USB** |

Two connectors, one macro, no diagnostic. A perfectly successful flash produces a completely
silent monitor if you're plugged into the other socket.

This bit us for real: COM27 is VID `0x303A`/PID `0x1001` — the S3's *native* USB — so the sketch
had to be built with `CDCOnBoot=cdc` or there would have been no output at all. The firmware now
prints which port it chose, so the board tells you itself:

```
 debug output is going to: native USB (USB CDC On Boot = Enabled)
```

Corollary: because `Serial` is a macro, never name a variable or struct field `Serial` in shared
code — it gets textually substituted and produces incomprehensible errors on ESP32 while
compiling cleanly on the Teensy.

---

## 7. The stale header copy — caught in the act

Mid-session, the Teensy build failed with `'gNackLimiter' was not declared in this scope`, even
though the declaration was plainly there.

The sketch folder held a **stale copy** of `interop_protocol.h`. The Arduino IDE requires headers
alongside the `.ino`, so the shared header is copied into both sketch folders — and copies drift.

This is the failure the whole sync mechanism exists to prevent, and it demonstrated itself
unprompted:

```
[copied]  esp32_wireless_bridge\interop_protocol.h -- was STALE (sha256 d9844071…)
[copied]  teensy_master_controller\interop_protocol.h -- was STALE (sha256 d9844071…)
```

Run `python tools/sync_headers.py` after **every** protocol edit, and re-upload **both** boards.
A protocol change applied to one side is worse than none: the boards still talk, they just
disagree, and the symptom looks like a wiring fault.

Second safety net at runtime: `RADIO_READY` carries the protocol version and CRC check value, and
the brain compares them on the first frame of every session.

**Do not** use `#include "../shared/interop_protocol.h"`. It compiles on ESP32 and fails on
Teensy — the worst outcome, because ESP32 succeeding first gives false confidence.

---

## 8. Smaller traps, each of which cost time

| Trap | Detail |
|---|---|
| **Teensy RX buffer is 64 bytes** | 0.7 ms of headroom at 921600. A ~1 KB scan response takes 11 ms. Overflow is **silent** — the ISR discards with no error flag. `addMemoryForRead()` is mandatory; we use 8 KB = 89 ms. |
| **`WiFi.disconnect(a, b)` blocks** | Third parameter defaults to a 100 ms timeout polled with `delay(5)`. Always pass `0`. A blocking delay hiding inside an innocuous call. |
| **`WiFi.SSID(i)` returns a `String`** | 30 heap allocations per scan. The allocation-free path is `(const wifi_ap_record_t*)WiFi.getScanInfoByIndex(i)` — same data, no heap. |
| **`LED_BUILTIN` on S3 isn't a GPIO** | Defined as `SOC_GPIO_PIN_COUNT + PIN_RGB_LED` — an offset-encoded addressable RGB pin. `digitalWrite` does nothing visible and reports no error. Use `rgbLedWrite()`. |
| **`setRxBufferSize()` after `begin()`** | Silently does nothing and returns 0. Must be called before. |
| **ESP32 `begin()` takes RX before TX** | `begin(baud, config, RX, TX)` — reversed compiles fine and produces a dead link. |
| **`arduino-cli` caches aggressively** | A compile can pass against stale objects after you've broken the header. Use `--clean` when verifying. |
| **`scanComplete()` keeps returning the count** | Until `scanDelete()` clears it — re-arm without deleting and you instantly "complete" a scan you never started. |
| **Scan record pointers are invalidated** | `scanDelete()` and the next `scanNetworks()` free them. Copy before deleting. |

---

## 9. Flashing the Teensy when the official path refuses

Covered in §4 as a toolchain fight. Here is what building the replacement actually taught.

### The 134-baud trick

A Teensy running a USB-Serial sketch jumps into its HalfKay bootloader when the host opens its
port at **134 baud**. No button, no Loader, no IPC. This is the escape hatch that makes automated
flashing possible when the normal path is broken, and it works from any language that can open a
serial port. Confirmed working: the COM port vanishes the instant the port closes.

### The address base was deduced, not looked up

A Teensy 4.x `.hex` is based at `0x60000000`. HalfKay's address field is **three bytes** — 24
bits, a 16 MB ceiling. `0x60000000` cannot fit in it, therefore wire addresses must be
flash-relative and the base must be subtracted.

That conclusion comes from the *width of the field*, which is why it can be trusted without a
datasheet. Windows then confirmed the packet size independently: `OutputReportByteLength` came
back as **1089** = 1088 payload + 1 report ID, exactly as computed.

### Two bugs worth keeping

**`ctypes` argument widths.** The first run died with `OverflowError: int too long to convert`. I
had declared `restype` but not `argtypes`, so ctypes defaulted arguments to C `int` — 32 bits —
and a 64-bit `HDEVINFO` would not fit. That error was *loud*. **The same mistake on a different
value silently truncates a pointer and hands the kernel garbage.** Always declare `argtypes` on
Win32 calls, not just `restype`.

**HalfKay needs pacing.** The flasher failed at block 6 of 85, every time, with
`ERROR_GEN_FAILURE`. HalfKay commits each block to flash before it can accept the next, and it
has no flow control to say so — it simply refuses the write. Writing as fast as `WriteFile`
returns is too fast. A 2 ms gap between blocks plus a few retries fixed it completely: **85 blocks
in 0.73 s, zero retries needed.**

Note the shape of that bug. Nothing was wrong with the protocol, the addresses, or the packet
layout — all of which I had carefully verified. It was a timing assumption I never thought to
question, and no amount of checking the *format* would have found it.

---

## 10. Adding a display without breaking the link

The OLED is where a comfortable assumption turned out to be wrong in an instructive way.

### What I believed, and why it was wrong

I claimed a 28 ms blocking I²C refresh could drop bytes mid-frame, since 28 ms is over half the
50 ms inter-byte timeout. **Both halves of that were wrong.**

Serial1 receive on a Teensy 4 is entirely ISR-driven — `IRQHandler()` fills the ring from
interrupt context no matter what `loop()` is doing. Blocking `loop()` delays the **parser**, not
**reception**. And the 50 ms timeout cannot be tripped by a slow reader either, because of the
fix made earlier that same day: `iop_parser_tick()` only runs when the receive queue is already
empty. The ceiling is the 8 KB ring's 89 ms, not 50 ms.

### The real constraint, which is subtler

This sketch measures round-trip latency with the Cortex-M7 cycle counter at 1.67 ns resolution —
mean 293 µs, σ 4.06 µs. A blocking refresh landing between sending a request and reading its
response gets **recorded as latency**. A 21 ms refresh is a **77-sigma outlier injected into the
exact statistic the display exists to show.** The display would corrupt its own measurement.

That is a completely different failure from the one I designed against, and it produces a
plausible-looking wrong number rather than an obvious error.

### The two rules that follow

1. **Never touch I²C while a request is in flight.** `gPending.active` gates every transfer.
2. **Push one tile row per iteration, not the whole screen.** u8g2 renders into a 1 KB RAM buffer
   with no I²C at all; `updateDisplayArea()` then pushes an 8-pixel strip. One row is 128 data
   bytes and Teensy 4's `Wire` buffer is 136, so a row is a **single transaction of ~2.7 ms**
   rather than one 21 ms blast.

### It was measured, not assumed

| | mean | σ | max | lost |
|---|---|---|---|---|
| Before the OLED existed | 295.130 µs | 6.018 µs | 312.805 µs | 0 |
| With OLED, 200 samples | 293.013 µs | 4.173 µs | 302.271 µs | 0 |
| With OLED, 500 samples | 293.023 µs | 4.058 µs | 299.015 µs | 0 |

**The decisive column is `max`.** One tile row is ~2.7 ms. If the gate had leaked even once
across 701 requests, there would be a ~3000 µs outlier sitting in it. Max was 299 µs — so the
gate held 701 out of 701 times. σ actually *improved*.

### Display gotchas

- **0.96" is SSD1306; 1.3" is SH1106.** Same address, same command set, so I²C cannot tell them
  apart — but the SH1106 has 132 columns of RAM behind a 128-pixel panel. Wrong driver gives a
  2-pixel shift and a garbage stripe. The way to settle it is to render with both and look.
- **Power from 3.3 V.** The module's pull-ups tie to *its* VCC, so a 5 V module drives the bus to
  5 V into pins that are not 5 V tolerant — without you ever running a 5 V wire to the Teensy.
- **`setClock()` quantizes to three values.** Under 400000 gives 100 kHz; asking for 700 kHz
  silently gives 400. The bus also runs ~7% fast off the 24 MHz LPI2C clock, so "400 kHz" is
  really 428.6 kHz.
- **`Wire::endTransmission()` calls `yield()`**, and `yield()` dispatches `serialEvent1()`. The
  link parser can therefore be drained *during* an I²C transfer. Not needed here, but it is a
  real escape hatch almost nobody uses.

---

## 11. The console was the worst offender all along

Found by review, not by failure — which is why it is worth recording.

The Teensy core defines `TX_TIMEOUT_MSEC 120` and spins on it. A single `printf` blocks up to
**120 ms** when the host is connected but not draining — a serial monitor scrolled up, minimised,
or just slow. The receive ring holds **88.9 ms**.

```
120 ms of possible blocking  >  88.9 ms of buffered headroom
```

The ESP32 side is worse: `HWCDC::write` blocks ≥100 ms and is *unbounded* under a trickling host,
against only 22.2 ms of headroom. And output is densest exactly when something interesting is
happening — a scan prints one line per network.

**The diagnostics could destroy the thing they diagnose.** Fixed in v1.1: the ESP32 calls
`setTxTimeoutMs(0)`, and the Teensy checks `availableForWrite()` before the dense paths and drops
the line instead of stalling. Dropped lines are counted in `stats`, so the mitigation is never
silent. Losing a log line is a fair trade for never losing a frame.

---

## 12. Diagnostic recipes

Commands that actually resolved things, worth keeping.

**Which board is on which port**

```bash
arduino-cli board list --format json
```

VID `0x303A`/PID `0x1001` = ESP32-S3 native USB. VID `0x16C0` = Teensy (see the PID table above).

**Is the ESP32 alive but deaf?** Watch its heartbeat for >10 s. `frames=0 discarded=0` while the
Teensy is transmitting proves nothing is arriving electrically.

**Which direction is broken?** Reset the ESP32 while watching the Teensy. If the wiring works,
its unsolicited `RADIO_READY` frame appears. Silence means the ESP→Teensy direction is dead.

**Is the protocol itself sound?** Both sketches run `iop_selftest()` at boot. If it fails, the
problem is in `interop_protocol.h` or how it was copied — **not** your wiring. That distinction
is the entire reason it exists.

**Real link quality**

```
bench 200
```

Measured on the actual hardware, cycle-accurate via the Cortex-M7 DWT counter:

```
samples 200 · lost 0 · min 279.148 µs · mean 297.768 µs · max 320.290 µs · σ 7.403 µs
```

12 bytes at 921600 8N1 is 130.2 µs of pure wire time, so ~168 µs is turnaround. σ of 7.4 µs —
2.5% of the mean — is a healthy, low-jitter link. The spread matters more than the mean: same
average with a 900 µs max would mean a periodic stall.

---

## 13. If you're stuck, start here

| Symptom | Look at |
|---|---|
| Nothing from the ESP32's USB port | Wrong connector — see §6, USB CDC On Boot |
| `LINK FAILED`, no PONG | TX↔RX not crossed (§1), then GND, then baud |
| `discarded = 0` both sides | Electrical. No wire, wrong pins, or TX→TX |
| Discarded climbing, no valid frames | Baud mismatch |
| Bad CRC on *every* frame, same delta | Stale header copy (§7) — sync and reflash **both** |
| PING fine, scans randomly fail | RX buffer too small (§8) |
| Self-test fails at boot | The header, not the wiring |
| Radio reports `BROWNOUT` | Power, not protocol. The rail sagged when the transmitter keyed up |
| WiFi "randomly drops" | `radio` — announcements > 1 means the radio is rebooting on its own |
| Teensy shows "unknown", no COM | It's in HalfKay (PID `0478`). Tap the button or use the flasher |
| `Unable find Teensy Loader` | §4 — use `tools/teensy_halfkay_flash.py` |

---

## What this cost, and what it bought

Fourteen distinct failures. Two were genuine protocol bugs that a 1.4-million-assertion test
suite missed. Two were tests that passed for the wrong reason. Five were toolchain and platform
traps with actively misleading error messages. Three were wiring and configuration. And two were
things I asserted confidently that turned out to be wrong — the false-start cost bound, and the
claim that a blocking display refresh would drop bytes.

That last category is worth its own note. Both wrong claims were *published* — one in this
repository's README, one in the protocol header — and both were caught by going back and checking
rather than by anything failing. A reference document that is confidently wrong is more dangerous
than one that is silent, because it gets believed.

The pattern worth taking away: **every one of the software failures was silent.** No exception,
no error code, no log line — a stale header still compiles, a swallowed frame looks like a
timeout, a straight-through UART looks like a dead link, and a flash to the wrong `Serial` looks
like a dead board.

That's why this protocol counts discarded bytes, prints a self-test at boot, announces its own
version on the wire, and reports the raw 802.11 reason alongside its cooked status. Every one of
those was added *after* something failed silently. They are not defensive programming for its own
sake — each is a scar.

---

# Part two: the mesh session

Five more failures, from putting an ESP-NOW mesh and a second ESP32-S3 sensor node underneath the
same UART link. The pattern from part one held — **all five were silent** — but three of them were
worse than silent: they produced confident, well-formatted output that happened to be wrong.

Numbering continues from above.

---

## 14. Hand-counted offsets into a struct we already shared

**Symptom:** a live accelerometer, sitting still on a desk, streaming to the brain at 200 Hz,
decoded as nothing at all:

```
reported:   a = 0  0  0        g = 0  0  0        lat = 1 µs
truth:      a = -216 456 17340 g = 467 158 -82    lat = 507 µs
```

**Cause:** the Teensy decodes relayed node payloads **byte by byte** — it cannot cast the buffer
to `MeshSampleMsg*`, because the payload sits at an arbitrary offset inside a larger frame and a
packed struct at an odd address is an unaligned access. Byte-by-byte decoding needs offsets, and
the offsets were counted by hand. The field widths were mis-added by two:

| Field | Hand-counted | Actual | What the wrong offset actually reads |
|---|---|---|---|
| `type` / `mode` / `seq` / `t_first_us` | 0 / 1 / 2 / 4 | 0 / 1 / 2 / 4 | correct |
| `int_to_send_us` (u16) | **10** | **8** | the `count` and `dropped` bytes |
| `count` (u8) | **12** | **10** | the low byte of `samples[0].ax` |
| `samples[]` | **14** | **12** | two bytes into the sample array, and every sample after it |

**Why it's so confusing: nothing failed.** The CRC passed — the frame *was* intact. The length
checks passed, the bounds checks passed, no NACK was raised, no counter moved on any of the three
boards. The link was genuinely healthy; only the meaning was wrong.

The single clue was that the numbers were not **plausible**. A real accelerometer at rest must see
roughly 16384 counts of gravity on one axis (the correct decode reads `az` ≈ 17340, about 1 g); an
all-zero reading from a live sensor is physically impossible. But implausibility is a far weaker
signal than a failed assertion — it only works if you already know what the answer should look
like *and* you are paying attention at the moment it scrolls past.

`lat = 1 µs` is the tell that cracked it. In interrupt mode there is one sample per packet, so
`count` = 1 and `dropped` = 0; the two bytes at offset 10, read as a little-endian `uint16`, are
exactly `0x0001`. The "latency" was the count field wearing a hat. An impossibly fast number is
easier to disbelieve than an impossibly empty one.

**The fix — define the offsets once, next to the struct, and make the compiler check them.**
`offsetof()` is not reliably available in every Arduino translation unit, and duplicating the
arithmetic across three sketches is what caused the bug in the first place. So `mesh_protocol.h`
now carries both the constants and a compile-time proof that they match the real layout:

```c
#define MESH_SAMPLE_OFF_LATENCY   8u   /* uint16 -- int_to_send_us */
#define MESH_SAMPLE_OFF_COUNT    10u
#define MESH_SAMPLE_OFF_SAMPLES  12u
#define MESH_SAMPLE_STRIDE       12u   /* 6 x int16 */

typedef char mesh_offsets_are_correct[
    (sizeof(MeshSample) == MESH_SAMPLE_STRIDE &&
     ((char *)&((MeshSampleMsg *)0)->int_to_send_us - (char *)0) == MESH_SAMPLE_OFF_LATENCY &&
     ...) ? 1 : -1];
```

A negative array size is the portable pre-C11 static assert and it works in every toolchain this
project targets. Add, move or resize a field now and **all three boards fail to build** instead of
quietly decoding garbage.

**The principle:** never hand-count offsets into a packed struct you already share in a header —
derive them, and make the compiler check. Note the shape of this one: the shared header existed,
was correct, and was included on all three boards. The bug happened anyway, because the decoder
re-derived by hand what the header already knew. A single source of truth stops being one the
moment somebody copies a fact out of it.

---

## 15. A half-finished constant migration reported success as failure

**Symptom:** every **successful** mesh peer-add and every successful `mesh send` printed `FAILED`
on the Teensy console — while the hub added the peer, the roster grew, and the node's data started
arriving exactly as asked.

**Cause:** a new status range, `IOP_MESH_STATUS_*`, was introduced (`OK = 0x00`) to replace a
constant borrowed from another subsystem. The ping reply was converted. `MESH_PEER_RESP` and
`MESH_SEND_RESP` were not.

| Response | Hub sends | Brain checks for | Result |
|---|---|---|---|
| `MESH_PING_RESP` | `IOP_MESH_STATUS_OK` (0x00) | `IOP_MESH_STATUS_OK` (0x00) | correct |
| `MESH_PEER_RESP` | `IOP_MESH_STATE_READY` (**0x01**) | `IOP_MESH_STATUS_OK` (0x00) | success reported as failure |
| `MESH_SEND_RESP` | `IOP_MESH_STATE_READY` (**0x01**) | `IOP_MESH_STATUS_OK` (0x00) | success reported as failure |

**Why it's so confusing:** nothing crashed, nothing NACKed, no counter moved. Two independently
compiled programs simply disagreed about what `0x01` meant, and the compiler checks neither side
against the other. Worse, `0x01` is not a garbage value in the new vocabulary — it is
`IOP_MESH_STATUS_BUSY`. The hub was not emitting nonsense; it was emitting a perfectly valid
status that happened to be a lie. A stale constant whose old value collides with a *live* value in
the new range will never look like corruption.

**The fix:** convert every site, and record why in the header next to the constants, so the next
person adding a status byte reads the story before the list.

**Two lessons:**

1. **Introducing a constant is not the change — converting every site is the change.** A partial
   conversion is strictly worse than none, because it looks done. The new names were present, the
   header comment explained them, and the migration was mentally filed as complete.
2. **Test the success path.** This project tested failure paths religiously — injected noise,
   truncated frames, oversize headers, corruption that cancelled itself out — and still shipped a
   bug that can only appear when everything **works**. Failure-path testing is the interesting
   part, and it is not sufficient.

---

## 16. The debug console dominated the latency measurement, by 5×

**Symptom:** the UART round trip degraded badly, with no protocol change that could explain it —
and stayed degraded across reflashes.

**Cause:** the ESP32-S3 sketches were flashed with an FQBN carrying `PartitionScheme=min_spiffs`
but **without** `CDCOnBoot=cdc`. That option defaults to *Disabled*, which makes `Serial` map to
UART0 on GPIO43/44 instead of the native USB — §6 again, wearing a new costume. The hub prints one
line per handled command, and those lines were now going to a **115200-baud hardware UART with
nothing draining it**, so `write()` blocked.

| Console destination | mean | max | σ |
|---|---|---|---|
| Native USB CDC (correct) | 435.94 µs | 635.40 µs | 42.39 µs |
| UART0, nothing draining it | 2261.35 µs | 10782.27 µs | 3894.08 µs |
| **Factor** | **5.2× worse** | **17× worse** | **92× worse** |

**The arithmetic that confirmed it:** 300 pings × ~30 bytes × 10 bits ÷ 115200 baud = **0.78 s** of
pure UART time, and the benchmark spent about **0.68 s**. The console was not a contributor to the
measurement — at that point it very nearly *was* the measurement.

**Two compounding traps, and the second one is the reason it survived a reflash:**

1. **The FQBN silently changes where `Serial` goes.** No error, no warning; the board simply
   appears dead on USB. Same trap as §6, but hidden inside a build command instead of a board
   menu, where nobody thinks to look for a menu.
2. **`arduino-cli` upload can upload a *cached build from a different FQBN*.** Compiling with the
   right FQBN and uploading with a shorter one gets you the old binary, cheerfully. The proof was
   the binary size: **900573 → 905593 bytes** the moment the FQBN was genuinely applied. Watch the
   byte count; it is the only thing that tells you a rebuild really happened.

Always compile and upload with **byte-identical** FQBN strings.

**The principle:** treat your instrumentation as part of the system under test. §11 made this
argument about the link — the console can block long enough to destroy the frames it is reporting
on. This is the same claim one level up: the console can destroy the *number*. A dead link
announces itself; a wrong number is patient, well-formatted, and gets quoted.

---

## 17. A confident diagnosis that was simply wrong

**Symptom:** the latency regression of §16 — with a plausible suspect already standing in the
room. The hub relayed **every** ESP-NOW packet to the brain unconditionally, 202 frames a second
the brain had never asked for and could not decline.

That was written up as the cause. With a table.

**Then the A/B was run:** the same 300-ping benchmark at three subscription settings, same boards,
minutes apart.

| Stream setting | min | mean | max | σ |
|---|---|---|---|---|
| off — relaying nothing | 412.99 µs | 2248.98 µs | 10752.05 µs | 3899.88 µs |
| all — 202 frames/s | 413.31 µs | 2241.67 µs | 11177.01 µs | 3690.16 µs |
| 1-in-20 | 412.93 µs | 2259.34 µs | 10796.37 µs | 3894.19 µs |

**Identical.** The three means span less than 0.8% — well inside run-to-run noise — and turning
the firehose completely **off** changed nothing whatsoever. The relay was never the cause. §16 was.

**What saved it, and it is worth naming:** the A/B was cheap *only because the subscription had
already been built*. With a switch on the suspect, a wrong hypothesis became falsifiable in one
sitting; without it, testing the theory would have meant a reflash, and a theory that expensive to
test tends to get believed instead.

**Be precise about what was wrong, though.** The unconditional relay was a genuine **design**
error: deciding how much telemetry the application wants is policy, and policy belongs on the
brain, not on the radio. Fixing it was right and the fix stands. It simply was not the performance
bug it was blamed for. Two true statements — "the relay is architecturally wrong" and "the latency
is bad" — were welded into a third that was false, and nothing about the reasoning felt weak at
the time.

**The principle:** a plausible mechanism plus a real symptom is not evidence of causation. Build
the switch that lets you turn your suspect off, then measure. Until you can turn it off you do not
have a diagnosis — you have a story that fits.

---

## 18. Events don't reach a listener that wasn't listening

**Symptom:** `mesh ping` refused with **"no live node known"** while the hub was tracking a live
node perfectly and could name its MAC on request.

**Cause:** the brain learned node MACs from exactly two sources — `MESH_NODE_EVENT` (`NODE_SEEN`)
and relayed sample data. Both are **pushed**, both had already happened, and the relay stream
defaults to **off**. A Teensy that reboots — or is reflashed while the hub keeps running, which is
every single iteration of this session — has missed every announcement there will ever be, and has
no way to learn a single address.

**Why it's so confusing:** nothing is broken. Every board is healthy, the fleet is intact, and the
hub's own status output shows the node alive with its packet count climbing. The fault lives
entirely in the brain's *model* of the world, which is empty for a reason no counter records: it
wasn't in the room when the announcement was made.

**The fix:** `MESH_NODES_REQ` / `MESH_NODES_RESP` (`0x3E` / `0x3F`) — a poll that returns the whole
roster, `[count]` then count × `[mac 6][alive][packets 4]`. `mesh nodes` on the console re-anchors
the brain against the hub's truth at any moment, from a cold start, with no history required.

**The principle:** event-driven state needs a polled re-anchor path, or it is broken for whoever
arrives late — and on a bench, *somebody is always arriving late*.

And note the sting in the tail: this project already had that exact pattern. `WIFI_STATUS_REQ`
exists for precisely this reason — push the change, poll the truth — and it was written months
before the mesh. The lesson had been learned once, in one subsystem, and never generalised into a
rule. A principle you have applied but never *named* will not be applied again.

---

## 19. The shell ate the escape — six times now

Not a firmware bug. A workflow one, and by raw count the most-repeated failure in this project.

**Symptom:** a patch script generated inside a bash heredoc produces a compile error nobody wrote:

```
error: missing terminating " character
```

with a C string literal apparently split across two lines in a source file that looked fine when
it was written.

**Cause:** `\n` inside a heredoc is consumed by the **shell**, before Python — or the compiler —
ever sees it. A `"...\n"` in the generator arrives in the `.ino` as a real newline *inside the
quotes*, which is a syntax error in C.

**Why it keeps happening:** each occurrence looks like a one-off typo, and the instinct every
single time is to escape harder — `\\n`, then `\\\\n` — which works just often enough to feel like
progress. It is not a typo. It is a layering error: two languages both claim the backslash, and
adding backslashes is negotiating with the wrong one.

**The fix is not to escape harder. Write the script to a FILE, then run the file.** One layer of
quoting instead of two or three, and the file is inspectable before it runs. This has now cost
time **six separate times** here; no individual instance was ever expensive enough to make anyone
stop and change the habit, which is exactly how a six-time bug happens.

**Closing note, from repair attempt number six.** One attempt to fix the escaping wrote a literal
**NUL byte** into a `.ino` where an escaped `'\0'` was intended. **It compiled** — a NUL character
literal is perfectly valid C — but it turned the source into a *binary* file as far as every text
tool was concerned. `grep` stopped printing matching lines and started answering
`Binary file ... matches`; diffs went opaque. A build that still succeeds while your search tools
quietly go blind on one file is a bad trade for a saved keystroke.

---

## 20. If the mesh is stuck, start here

Companion to §13, for the wireless half.

| Symptom | Look at |
|---|---|
| Node data decodes as all zeros, latency of 1 µs | §14 — hand-counted offsets. At rest `az` should read ≈17340 counts (about 1 g) |
| Every *successful* mesh command prints FAILED | §15 — the two boards disagree about a status byte |
| Latency several × worse with no code change | §16 — check the FQBN for `CDCOnBoot=cdc`, and check the binary size actually changed |
| You "fixed" it but the behaviour is identical | §16 — `arduino-cli` uploaded a cached build from a different FQBN |
| `mesh ping` says "no live node known" while the hub sees the node | §18 — the brain rebooted and missed the events. `mesh nodes` polls the roster |
| A node goes silent while `mesh stream off` and the brain still hears about it | Not a bug. Node events are never gated by the stream — that is the design |
| `missing terminating " character` in code you did not write that way | §19 — the heredoc ate the escape |
| One board obeys a broadcast command, another does not | Check the channel first. ESP-NOW peers must share one, and joining an AP hands that choice to the AP |

---

## What part two cost, and the one new thing it taught

Five more failures; **nineteen** in total. The breakdown:

- **Two produced plausible-looking wrong numbers** — hand-counted offsets (§14) and a benchmark
  dominated by its own logging (§16).
- **One reported success as failure** (§15), from a migration that was half done and filed as
  finished.
- **One was a confident, carefully-argued diagnosis** that a ten-minute experiment demolished
  (§17).
- **One was a hole in the brain's model of the world** (§18), already solved elsewhere in this
  same codebase and never generalised.
- **And one was the same shell-quoting mistake for the sixth time** (§19).

Part one's summary ended on the observation that every software failure was **silent**. Part two
adds a sharper version of that: three of these were *worse* than silent. A decoder printing
`a = 0 0 0`, a console printing `FAILED` after succeeding, and a benchmark reporting a mean five
times too high are all **articulate**. They produce output in the right format, at the right rate,
in the right units. Silence at least invites suspicion; a confident wrong answer recruits you as
its advocate — you quote it, you build on it, and in §17's case you write it up in a table.

The defence is not more assertions, because none of these violated one. It is knowing what the
answer should look like before you look — 1 g on a resting axis, a success that says success, a
latency that cannot be shorter than the wire time — and building the switch that lets you turn
each suspect off and measure again.
