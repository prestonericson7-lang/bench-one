# hmi_e32r40t — the 4.0 inch display node

ESP32-WROOM-32E on an E32R40T board. ST7796 480x320 with XPT2046 resistive touch.
**Classic ESP32, not an S3.**

## Build

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 hmi_e32r40t
```

No setup step. `build_opt.h` in this folder configures TFT_eSPI, so a fresh checkout builds
correctly and nothing global has to be edited first.

## Why `build_opt.h` exists

TFT_eSPI is normally configured by editing `User_Setup.h` **inside the installed library**. That
is a global, invisible edit: it silently reconfigures every other sketch on the machine, it is
not version controlled, and it is lost the moment the library updates.

Arduino passes every line of a sketch-local `build_opt.h` to the compiler as a flag, which does
the same job with none of those costs. The panel configuration lives next to the code that needs
it.

**Do not put comments in `build_opt.h`.** Every line becomes a compiler flag, including comment
lines, and the build fails with `unrecognized command-line option`. That is why this file exists
instead.

## Why not Arduino_GFX

His own working firmware for this board uses Arduino_GFX, and that was the first choice here too.
It does not build in this environment.

`GFX Library for Arduino` 1.6.7 guards its use of `ESP_INTR_CPU_AFFINITY_AUTO` on
`ESP_ARDUINO_VERSION_MAJOR < 3`. The installed core is 3.0.7 — major version 3, so the guard
lets the code through. But that symbol only arrives in arduino-esp32 **3.1**. The library's
version check is too coarse by one digit, and the result is a reference to a symbol that does
not exist.

```
Arduino_ESP32QSPI.cpp:58:23: error: 'ESP_INTR_CPU_AFFINITY_AUTO' was not declared in this scope
```

It fails inside the library, before any sketch code is reached, so no change to this sketch can
work around it. **His existing E32R40T firmware will not build in this environment either.**

Fixing it means moving a version, and both directions have a cost:

| Option | Cost |
|---|---|
| Bump esp32 core 3.0.7 → 3.3.11 | 3.0.7 is what the **tested ESP32-S3 radio baseline** was measured on, and arduino-cli holds one version per core. Bumping it invalidates that baseline until it is re-measured. |
| Downgrade GFX below the version that added this | Safer, but affects any other sketch using GFX. |

TFT_eSPI 2.5.43 is already installed, supports the ST7796 and the XPT2046, and builds cleanly on
3.0.7. It touches neither version, so it is the default here.

If the core is bumped later, the Arduino_GFX path can come back and this file becomes history.

## One thing TFT_eSPI made better, not just different

It drives the panel **and** the touch controller through one bus manager.

His bring-up on this board found that the Arduino_ESP32SPI backend locks the HSPI pins to the SPI
peripheral, after which nothing else can reach the XPT2046 sitting on the same three wires — the
fix was a shared `SPIClass` in transaction mode. With TFT_eSPI that arbitration problem does not
arise: one object owns both devices on the bus.

## Link wiring

The link is on connector **P4**, the one silkscreened `I2C` — **not** P2, the one silkscreened
`UART`. Full reasoning is in `README-QUESTIONS.md` at the repo root; the short version is that P2
is UART0 wired in parallel with the onboard CH340C through 100 ohm resistors, so with USB plugged
in a Teensy cannot pull this chip's RX below its input threshold.

| P4 pin | Signal | Teensy 4.1 #1 |
|---|---|---|
| 1 | 3.3 V | leave unconnected |
| 2 | GPIO32 | pin 29 (TX7) |
| 3 | GPIO25 | pin 28 (RX7) |
| 4 | GND | any GND |

Connectors are **1.25 mm pitch**. JST-PH 2.0 mm cables will not mate.

## `HMI_DISPLAY_ENABLED`

Set to `0` to build the link half with no graphics library at all. The link and the panel fail
for entirely different reasons, and being able to prove one while the other is unavailable is
what keeps a bring-up session moving.

## Status

Compiles clean. **Never run on hardware.** Everything below the link layer is unverified.
