# Firmware

> **Scope: this covers the four phase-one node sketches and the shared protocol header.** It predates
> the neural work and does not describe anything under `bench-one/tests/`, which is where every
> experiment since lives. Each test directory is self-contained and compiles on its own; see the
> top-level README. Nothing below is wrong, but it is not the whole firmware any more.

**No longer blocked.** Your tested source is in `original/`, the BENCH ONE build is in
`bench-one/`, and all four sketches compile.

```
$ bash tools/build_all.sh
building teensy1_master   ... OK    FLASH: code:115776
building teensy2_worker   ... OK    FLASH: code:18480
building esp32_radio      ... OK    Sketch uses 905593 bytes
building hmi_e32r40t      ... OK    Sketch uses 311825 bytes
4 passed, 0 failed, 0 skipped
```

---

## The one guarantee everything else rests on

`interop_protocol.h` is **byte-identical** to your tested file — md5
`f90495fd51b6ee308935efb7642636d2` in `original/shared/`, in `bench-one/shared/`, and in every
sketch folder. `sync_headers.py` fails loudly if that ever stops being true.

Your master controller has a **three-line diff**: one include and two calls.

```
$ diff original/teensy_master_controller/... bench-one/teensy1_master/...
22 lines added, 3 of them code.
```

Delete `bench_master.h` and those three lines and you have the file that produced the
53,530-frame, zero-error baseline. That is the acceptance test for this whole tree.

**Why so little.** The radio link, the parser, the pending-request slot and the console are
correct *because they were measured*. Every line added to that file is a line that has to be
re-proven on hardware. So the three new links and the entire fabric were built alongside it
rather than woven into it.

---

## Layout

```
bench-one/
  shared/              master copies — edit HERE, then run tools/sync_headers.py
    interop_protocol.h   your tested file. DO NOT EDIT.
    bench_protocol.h     5 new channels, ~60 commands. Strictly additive.
    bench_pins.h         every pin on every board, one file
    bench_ports.h        the port registry + a boot-time validator
    bench_link.h/.cpp    one framed UART link, instantiated per link
    bench_fabric.h/.cpp  the 74HC/MCP driver
    bench_master.h/.cpp  hub logic: 3 links + fabric + dispatch + console
    bench_display.h/.cpp the 2.4in TFT on SPI0, with runtime controller detection
  teensy1_master/      your sketch + 3 lines
  teensy2_worker/      new — sliced compute jobs
  esp32_radio/         your bridge, unchanged
  hmi_e32r40t/         new — 4.0in display node
  luckfox/             iop.py, bench.py, benchctl.py, orchestrator.py, S98orchestrator
  tools/               sync_headers.py, build_all.sh
```

## Build and flash

```bash
cd firmware/bench-one
python tools/sync_headers.py          # after ANY edit in shared/
bash tools/build_all.sh
```

**Compile and upload with the identical FQBN string.** Both traps in the ESP32-S3 board name have
already cost this project real time: without `CDCOnBoot=cdc`, `Serial` is UART0 on GPIO43/44 and
the board runs perfectly while saying nothing on USB; and `arduino-cli upload` can flash a cached
build produced under a *different* FQBN, giving symptoms that match neither source tree.

```
teensy1_master   teensy:avr:teensy41
teensy2_worker   teensy:avr:teensy41
esp32_radio      esp32:esp32:esp32s3:PartitionScheme=min_spiffs,CDCOnBoot=cdc
hmi_e32r40t      esp32:esp32:esp32
```

---

## The address space

Commands are namespaced per channel, but every new range is **globally unique anyway**. If CHAN is
ever corrupted — the parser's own docs record a false-start survival rate of 1 in 17,614 over 4 MB
of noise — a unique CMD still identifies the frame. Overlapping ranges would turn a corrupted
channel byte into a plausible frame on the wrong subsystem.

| Channel | Range | Link | Status |
|---|---|---|---|
| 0x00 TRANSPORT | 0x01–0x0F | all | existing |
| 0x01 WIFI | 0x10–0x1F | T1 ↔ ESP32-S3 | existing, **tested** |
| 0x02 BLE | 0x20–0x2F | T1 ↔ ESP32-S3 | existing, reserved |
| 0x04 MESH | 0x30–0x3F | T1 ↔ ESP32-S3 | existing, **tested** |
| **0x05 FABRIC** | 0x50–0x6F | any → T1 | new |
| **0x06 WORKER** | 0x70–0x7F | T1 ↔ T2 | new |
| **0x07 ORCH** | 0x80–0x8F | Luckfox ↔ T1 | new |
| **0x08 HMI** | 0x90–0x9F | T1 ↔ display | new |
| **0x09 ROUTE** | 0xA0–0xAF | stack ↔ stack | new |

`0x40–0x4F` is left empty on purpose. The handoff earmarked it for the orchestrator; the
orchestrator ended up at 0x80. The gap costs nothing and makes anything written against the old
plan fail with `UNKNOWN_CMD` instead of quietly landing on a real command.

---

## The 2.4 inch display

Confirmed a TFT. It lives alone on SPI0 while the fabric owns SPI1, so a refresh can never queue
behind a fabric transaction — and, more importantly, can never land between a request and its
response and get **recorded as latency**, which is the failure this project's bring-up log already
documents once.

The driver **identifies the controller at runtime**. The BOM says ST7789, but most 2.4in 240x320
SPI modules are ILI9341, and the wrong driver does not fail cleanly: it gives a shifted image or
wrong colours with no error. So it reads the ID register and picks.

If the module does not bring MISO out — many cheap ones do not — the read is impossible and it
falls back to ILI9341, reporting `ASSUMED` rather than `detected` on both the console and the
screen. That distinction is the point: if the image comes up shifted, knowing whether the
controller was read or guessed decides whether you suspect the driver or the wiring.

Only rows that changed are repainted. A full 240x320 repaint is 153,600 bytes, roughly 41 ms at
30 MHz; one row is well under a millisecond.

---

## SPI port auxiliaries

A module needs more than the bus and a chip select. Each of the eight SPI ports carries two more
signals, and they live on **different silicon because they point in different directions**:

| | MCP23017 pin | 74HC595 output |
|---|---|---|
| Direction | in or out | out only |
| Interrupt on change | yes, in hardware | no |
| Time to change one bit | ~120 us | **~2 us** |
| Atomic with others | 16 bits per transaction | **whole chain, one RCLK edge** |

Module **outputs** (interrupt, data-ready, busy) go to the expander, the only thing that can raise
a flag without being polled. Module **inputs** (reset, chip enable) come from the shift chain,
sixty times faster.

Both answer the ordinary pin calls, so no new command was needed:

```
index 0  =  CTL, an OUTPUT, on 74HC595 chip 1
index 1  =  IRQ, an INPUT, on MCP_A
```

```bash
./benchctl.py reset P3     # pulse a module's reset
./benchctl.py irq P3       # read its interrupt line
```

Eight ports x one interrupt is sixteen expander pins, which is exactly one MCP23017. The
arithmetic lands rather than being forced, and the second bank stands ready for the modules that
have two interrupts.

Full specification, including connector pinouts and the bring-up jig, is in
[docs/04-port-map.md](../docs/04-port-map.md).

---

## What makes "plug in anything" true

Two commands, and they are the reason a module nobody has written a driver for is fully
controllable on the day it arrives:

```python
f.spi_xfer("P3", tx=[0x0F, 0x00])          # any SPI module, addressed by silk label
f.i2c_xfer(0x76, write=[0xD0], read=1)     # any I2C sensor, repeated START
```

Purpose-built commands exist for the things worth doing fast. These exist so nothing is ever
blocked on firmware.

Ports are named the way the silk names them. **The port map is read from the hardware**, not
duplicated in Python — there is no second copy to drift.

---

## Six guarantees the fabric driver keeps

1. **A chip select is never left asserted.** There is no public "select" call; selection happens
   only inside a transfer, which deselects on every path out.
2. **Two chip selects are never low at once.** Every address change is break-before-make. A '138
   whose address changes while enabled walks a low pulse across intermediate outputs — nanoseconds,
   and more than enough to clock a byte into an SPI flash chip nobody was addressing.
3. **The '165 chain is off MISO unless it is being read**, gated through a 74HC373 held
   transparent.
4. **An MCP23017 interrupt always reads both ports.** With MIRROR set, the condition does not
   clear until both are read; servicing one leaves INT asserted forever and interrupts stop
   system-wide with no error anywhere.
5. **The first ADC sample after a mux change is discarded, always.** The error is proportional to
   the *previous* channel, so keeping it makes every reading a function of its neighbour.
6. **Every operation distinguishes "not there" from "failed."** A '595 acknowledges nothing, so a
   write into an empty socket succeeds — where the hardware cannot tell us, the port table does.

It does no dynamic allocation, never blocks longer than one transaction, and **never retries**. A
retry hides a fault during bring-up, and this fabric is being characterised, not shipped.

---

## At the bench

```bash
python3 benchctl.py sys        # whole-stack health, one round trip
python3 benchctl.py scan       # what is on the I2C bus
python3 benchctl.py ports      # the port map, from the hardware
python3 benchctl.py selftest   # the L0-L7 ladder
python3 benchctl.py watch      # stream unsolicited events
python3 benchctl.py mark 3     # pulse the analyser marker
python3 benchctl.py reset P3      # pulse a module's reset line
python3 benchctl.py irq P3        # read its interrupt line
python3 benchctl.py panel         # walk the bring-up jig end to end
python3 benchctl.py bench crc16 100000
```

On the Teensy console: `bench`, `fabric`, `scan`, `ports`, `selftest`, `mark`, `park`. Your
existing commands are untouched.

---

## Three design decisions worth knowing before you read the code

**Jobs run in slices, never to completion.** A job request is *accepted*; completion arrives later
as an unsolicited event. One outstanding request per link means a two-second job run inline would
make the worker unreachable for two seconds — no cancel, no status, no way to tell it from a
crash. Head-of-line blocking is the failure; slicing is the fix.

**Events default to OFF.** This is Defect 1 from your own baseline applied to three new links.
That defect was an unconditional relay pushing 202 packets/second at a node that never asked and
could not decline. Whether telemetry flows upward, and how much, is a decision for whoever reads
it.

**A HELLO re-anchors the event counter instead of counting as loss.** A node that rebooted
legitimately restarted its counter. Reporting that as "127 events missed" the first time you
reflash a board is the false alarm that teaches people to ignore a counter forever after.

---

## Status

| | |
|---|---|
| Compiles | all four sketches, clean |
| Protocol codec | **runs and passes**, CRC check `0x6F91` reproduced |
| Header identity | verified byte-identical every build |
| Run on hardware | **never** |

Everything except the Python codec is a well-argued hypothesis. The comments trace to datasheets,
not to your bench.
