# RTL-SDR Pentest — display UI (E32R40T)

The UI half of the RTL-SDR pentest handheld. This ESP32-WROOM-32E board (LCDWIKI
E32R40T, ST7796 480x320) **runs the UI**: menus, screen layouts and the spectrum
waterfall. It receives semantic data (spectrum / detections / telemetry) and
forwarded button events over UART2, and sends SDR control commands back to the
Teensy engine.

- Link: UART2, GPIO25=TX / GPIO32=RX, 921600 8N1, on the P4 "I2C" connector.
- Display backend: TFT_eSPI, configured entirely by `build_opt.h` here (no library edits).
- `link_proto.h` is the shared protocol — keep it byte-identical to the copy in
  `../rtlsdr-pentest`.

Full docs, wiring and the bench bring-up checklist: `../rtlsdr-pentest/README.md`.

Build/flash:
```
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload  --fqbn esp32:esp32:esp32 -p <PORT> .
```
