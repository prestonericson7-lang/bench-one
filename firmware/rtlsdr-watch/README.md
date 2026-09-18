# RTL-SDR Pentest — watch controller (ESP32-S3 AMOLED, "WEDJAT")

The **controller** node of the RTL-SDR pentest handheld. An ESP32-S3 AMOLED smart-watch
board reaches the SDR wirelessly and drives the whole system: it owns the full control
surface (tune, mode, gain, bands, capture, GPIO-RF TX) and mirrors the engine's
spectrum / telemetry / detections back. Its capacitive touch works, so it — not the
E32R40T — is the real UI; the E32R40T is demoted to a bridged screen.

- **Link:** ESP-NOW, Wi-Fi channel 1, broadcast peer, packet = `[4-byte "RSDL" magic][one lp-frame]`.
  The E32R40T is the **bridge**: it speaks `link_proto.h` down its P4 UART to the Teensy
  and mirrors it over ESP-NOW to this watch. The Teensy stays unaware of the watch.
- **Board:** ESP32-S3, 16MB flash, 8MB OPI PSRAM. Display Arduino_GFX `Arduino_CO5300`
  410×502 AMOLED (QSPI); touch FT3168; PMU AXP2101; IMU QMI8658; RTC PCF85063;
  SD_MMC (1-bit); mic ES7210. Pin map in `pin_config.h`.
- `link_proto.h` is the shared protocol — keep it **byte-identical** to the copies in
  `../rtlsdr-pentest` and `../rtlsdr-display`.

## Libraries (installed separately, not vendored)

Per this repo's convention only sketch source is committed. Install into your Arduino
libraries folder:

- **lvgl 9.x** — and place the committed `lv_conf.h` (here) at `<libraries>/lv_conf.h`,
  i.e. one level *above* the `lvgl/` folder.
- **Arduino_GFX** (with `Arduino_CO5300` support), **SensorLib** (AXP2101 / QMI8658 /
  PCF85063), the ESP32 Arduino core 3.3.x.

> **lv_conf.h matters — it holds a fix, not just config.** `LV_USE_STDLIB_SPRINTF` is set
> to `LV_STDLIB_CLIB`, **not** the default `LV_STDLIB_BUILTIN`. With the builtin printf and
> `LV_USE_FLOAT 0`, a `%f` in `lv_label_set_text_fmt` is compiled out and does **not**
> consume its `double` arg, so any following `%s` reads a shifted `va_arg`, dereferences the
> double's bits as a pointer, and panics (Core 1 LoadProhibited). This rebooted the watch
> whenever SDR telemetry arrived. The CLIB backend routes formatting through newlib's
> `vsnprintf`, which handles `%f` correctly.

## Build / flash

```
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB --libraries <path-with-lv_conf.h-and-lvgl> .
arduino-cli upload  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB -p <PORT> .
```

Full system wiring and the bench bring-up checklist: `../rtlsdr-pentest/README.md` and
`../rtlsdr-pentest/SYSTEM-WIRING.md`.

The `disabled/` folder holds apps kept out of the build (Arduino only compiles the sketch
root, not sub-folders); they are parked source, not dead code.
