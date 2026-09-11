# 02 — Wiring the Stack (4-node topology + exact pinout)

Live artifact: https://claude.ai/code/artifact/f1d28183-cc92-4d7e-b338-ac492dd76271
Source: `hardware/wiring-the-stack.html`

All three UART links: **921600 8N1, 3.3 V both ends, direct wire (no shifter), TX→RX, shared GND.**

## Link A — Luckfox ↔ Teensy1 (orchestrator) [NEW]
| Signal | Luckfox Pico Mini B | Teensy 4.1 #1 |
|---|---|---|
| Lk TX → T1 RX | pin 12 · GPIO1_D0 · UART3_TX_M1 | pin 7 · RX2 (Serial2) |
| T1 TX → Lk RX | pin 13 · GPIO1_D1 · UART3_RX_M1 | pin 8 · TX2 (Serial2) |
| GND | pin 2 or 21 | any GND |

Enable UART3 on the Luckfox (device tree / luckfox-config); it is NOT a console by default.
Console stays on UART2 (pins 4/5). Port shows up as `/dev/ttyS3`.

## Link B — Teensy1 ↔ ESP32-S3 (radio) [UNCHANGED — tested]
| Signal | Teensy 4.1 #1 | ESP32-S3 |
|---|---|---|
| T1 TX → ESP RX | pin 1 · TX1 (Serial1) | GPIO18 (RX) |
| ESP TX → T1 RX | pin 0 · RX1 (Serial1) | GPIO17 (TX) |
| GND | any GND | any GND |

## Link C — Teensy1 ↔ Teensy2 (worker) [NEW]
| Signal | Teensy1 (master) | Teensy2 (worker) |
|---|---|---|
| Master TX → Worker RX | pin 14 · TX3 (Serial3) | pin 0 · RX1 (Serial1) |
| Worker TX → Master RX | pin 15 · RX3 (Serial3) | pin 1 · TX1 (Serial1) |
| GND | any GND | any GND |
Worker pins 2–41 stay free for compute jobs.

## Fabric bus (off Teensy1) — see 03-logic-fabric.md for the full build
- SPI0: SCK 13, MOSI 11, MISO 12  (pin 13 is also onboard LED — blinks on traffic, harmless)
- I2C (Wire): SDA 18, SCL 19  (2.2k pull-ups ONCE here)
- 74HC138 address: 2/3/4 ; global enable gate: 33
- TFT: CS 10, DC 9, RES 6, BLK 5 (PWM) ; encoder A 22 / B 23 / PUSH 21 ; KEY0 20
- 595 RCLK / 165 LD: dedicate two GPIO (e.g. 16 / 17)

## Power
5 V rail (bank/PSU) → Teensy1 VIN, Teensy2 VIN, ESP32 5V, Luckfox VBUS(pin1). Each board makes
its own 3.3 V. Fabric 3.3 V from Teensy1 3V3 (≤250 mA) or a dedicated buck. Star GND. Cut the
VUSB–VIN pad on both Teensys. Bulk 220–470 µF at the ESP (WiFi TX brownout).

## Decisions (flip any)
| Decision | Chosen | Flip if |
|---|---|---|
| Topology | Luckfox→T1→{ESP,T2} | want Luckfox on the ESP directly, or worker on the Luckfox |
| Fabric voltage | 3.3 V everywhere | you want a 5 V fabric (shifters go inline) |
| Bus ownership | T1 masters I2C+SPI; others ask over UART | want Luckfox directly on I2C (its I2C3 = pins 14/15) |
| Fabric leaves | unassigned until you name loads | tell me the peripherals and every expander pin gets assigned |
