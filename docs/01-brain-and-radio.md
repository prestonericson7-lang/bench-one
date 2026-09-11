# 01 — Brain and Radio (the baseline protocol)

His existing, tested ESP32-S3 ↔ Teensy 4.1 UART protocol. **This is the foundation the whole
stack speaks. Do not reimplement it from this summary — extend the real source files.**

Live doc: https://claude.ai/code/artifact/8841f41b-2655-499b-844b-322cc6609b63
Reference impl (his files): `interop_protocol.h`, `esp32_wireless_bridge.ino`, `teensy_master_controller.ino`

## Wire
- Direct UART, **921600 8N1**, shared ground required.
- Teensy pin 1 (TX1) → ESP GPIO18 (RX); ESP GPIO17 (TX) → Teensy pin 0 (RX1). (Teensy Serial1.)

## Frame
`START(0xAA) | LEN_H | LEN_L | SEQ | CMD | PAYLOAD | CRC8`
- LEN = big-endian count of CMD+payload (never 0). Max frame 1024 B, max body 1019 B.
- CRC8 covers LEN..payload (START excluded). **poly 0x31, MSB-first, init 0x00, check 0xA2.**
  (This is NOT CRC-8/MAXIM despite a mislabel in the original spec — follow the parameters.)
- SEQ ties response to request; **0x00 is reserved = unsolicited event** (radio can speak
  unasked). Inter-byte timeout 50 ms, evaluated only when the RX queue is empty.
- Byte-at-a-time resync parser: WAIT_START → LEN_H → LEN_L → SEQ → BODY → CRC, with a SKIP
  state that drains exactly an over-long declared length instead of cascading false frames.

## Roles
**Mechanism on the radio, policy on the brain.** The ESP drives the 802.11 MAC and reports raw
facts (incl. the raw 802.11 reason code beside a friendly status); the Teensy decides what a
failure means and what happens next. The radio is replaceable — nothing in the frame is
Espressif-specific.

## Command map (existing)
| CMD | Name | Dir |
|---|---|---|
| 0x01/0x02 | PING / PONG | brain↔radio |
| 0x04 | RADIO_READY (unsolicited) | radio→brain |
| 0x10/0x11 | WIFI_SCAN_REQ / RESP | |
| 0x12/0x13 | WIFI_CONNECT_REQ / RESP | |
| 0x14 | WIFI_EVENT (unsolicited) | radio→brain |
| 0x15/0x16 | WIFI_STATUS_REQ / RESP (v1.1) | |
| 0x2x | reserved for BLE (milestone 4) | |
| 0xFE | NACK (err 0x01–0x06) | either |

## Extension plan for the stack (keep his walls intact)
Every new link is point-to-point, so the same header+CRC is reused verbatim. New ranges only:
- 0x30–0x3F worker jobs (Teensy1↔Teensy2)
- 0x40–0x4F orchestrator (Luckfox↔Teensy1)
- 0x50–0x5F fabric/IO
0x1x (WiFi) and 0x2x (BLE reserved) are never renumbered.
