# The ESP32-P4-NANO in the machine (phase 2)

Board facts (Waveshare's documentation, recorded 2026-09-26): ESP32-P4NRW32 — dual-core RISC-V at
360 MHz, 32 MB PSRAM in package, 16 MB flash; **100 Mbit RJ45** (IP101 PHY); USB 2.0 High-Speed OTG on a
USB-A port (host / device by jumper); a USB-C for flashing and console (the chip's USB-Serial-JTAG,
VID 303A:1001); audio codec + amplifier, microphone, 3.5 mm and an **MX1.25 speaker header**; TF slot;
40-pin header; an ESP32-C6 on board for Wi-Fi/Bluetooth. **No screen is owned** (no DSI panel), and the
8 Ω speaker is.

## Pins (two independent board-support sources agree; checked against the silkscreen on the day)

From `esp-cpp/espp` `components/esp32-p4-nano/include/esp32-p4-nano.hpp` (a BSP that runs this board)
and `shyndman/esphome-configs` `docs/esp32-p4-nano-pinout.md`:

| function | chip | GPIO |
|---|---|---|
| Ethernet RMII | **IP101GRI** on the P4's internal EMAC | REF_CLK 50 (50 MHz), MDC 31, MDIO 52, PHY reset 51, TXD0 34, TXD1 35, TX_EN 49, RXD0 29, RXD1 30, CRS_DV 28 |
| TF card, 4-bit SDMMC | | CLK 43, CMD 44, D0 39, D1 40, D2 41, D3 42 |
| audio | **ES8311** codec at I2C 0x18, **NS4150B** amplifier | I2C SDA 7, SCL 8; I2S MCLK 13, BCLK 12, WS 10, DAC out 11, mic in 9; amplifier enable (PA) 53 |
| USB Serial/JTAG (the USB-C, flashing + console) | | D− 24, D+ 25 |
| USB OTG 2.0 HS (the USB-A) | | D− 26, D+ 27 |
| 40-pin header GPIO | | 0–6, 20–23, 32, 33, 36–38, 45–48, 54, 83 (GPIO 34–38 shared with Ethernet TX / free per the note) |

## Role

Never compute (the memory rule: it is slower than every other processor here). Two things nothing else in
the machine has:

1. **The panel.** An ESP-IDF application serving one web page over its RJ45: type a question, it is
   queued to zynq1, the answer comes back and is shown; the speaker plays a chime when an answer is
   ready. It is the machine's front door for a phone or laptop on the same cable run.
2. **A network path for the PC, later.** The P4's USB-HS device mode can present a USB network adapter
   (NCM) to the PC and bridge it to its RJ45, giving the PC a way onto the machine's network without
   touching the PC's own Ethernet port. This needs the board's USB-A in device mode (jumper) and an
   A-to-A cable, and a bridge in firmware; it is a stretch item and is not on the critical path.

## Preconditions

- **eth1 on the FPGA boards** (`machine/zynq/ETH1.md`): the P4 plugs into FPGA #2's lower jack, which
  today's boot files leave unclocked. That change is tested on FPGA #1 first, from the console, and
  only after FPGA #1's first full boot has been reported.
- A small answer service on zynq1 (`machine-serve`, TCP 8094: a line in, the model's answer out, backed by
  `run_model`) — written with the panel, tested on the PC first with the engine's software model.
- ESP-IDF 5.x installed on the PC (about 1 GB), the board on its USB-C.

## Cables

RJ45 → FPGA #2 LOWER Ethernet jack (cable 9). USB-C → the PC's hub (cable 10) for flashing, console and
power. Speaker → the MX1.25 header marked SPK (cable 11; two wires, polarity irrelevant for a lone
speaker). Nothing on the 40-pin header, the TF slot or the camera connector.

## Bring-up, one build per question

1. Flash a blink-and-print over USB-C: the board answers on its console.
2. Ethernet up on the FPGA's eth1: `ping zynq2` from the P4's console, `ping` the P4 from zynq1.
3. The web page served; a question typed on a phone reaches `machine-serve` and the answer returns.
4. The chime through the speaker header.
5. Then, if wanted, the USB-NCM bridge.
