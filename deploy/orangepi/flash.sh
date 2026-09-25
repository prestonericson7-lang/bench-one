#!/bin/bash
# flash.sh -- program the car's microcontrollers from the Orange Pi (installed as /opt/car/flash.sh).
#
#   sudo /opt/car/flash.sh logger     # Teensy 4.1 CAN logger      <- car-can-logger.ino.hex
#   sudo /opt/car/flash.sh display    # Teensy 4.1 vent display    <- vent-display.ino.hex
#   sudo /opt/car/flash.sh climate    # ESP32-S3 climate node      <- vent-climate-node.ino.merged.bin
#
# Plug in ONLY the board you are flashing. The hub is paused while flashing (it would otherwise grab
# the port) and started again afterwards. Each image is checked against firmware/SHA256SUMS first.
set -euo pipefail
FW=$(cd "$(dirname "$0")" && pwd)/firmware
what=${1:-}
case "$what" in
  logger)  img=car-can-logger.ino.hex ;;
  display) img=vent-display.ino.hex ;;
  climate) img=vent-climate-node.ino.merged.bin ;;
  *) echo "usage: $0 logger|display|climate"; exit 1 ;;
esac
( cd "$FW" && grep -E " \*?$img\$" SHA256SUMS | sha256sum -c - ) || { echo "image $img failed its checksum"; exit 2; }

systemctl stop car-hub.service 2>/dev/null || true
trap 'systemctl start car-hub.service 2>/dev/null || true' EXIT

if [ "$what" = climate ]; then
  port=$(ls /dev/serial/by-id/*Espressif* 2>/dev/null | head -1)
  [ -n "$port" ] || port=$(ls /dev/ttyACM* 2>/dev/null | head -1)
  [ -n "$port" ] || { echo "no ESP32 serial port found"; exit 3; }
  ET=$(command -v esptool || command -v esptool.py)
  echo "flashing $img to $port"
  "$ET" --chip esp32s3 --port "$port" --baud 921600 write_flash 0x0 "$FW/$img"
else
  # -s: soft-reboot a running Teensy into its bootloader; -w: wait for it; -v: verbose
  echo "flashing $img (if nothing happens within 10 s, press the Teensy's program button once)"
  teensy_loader_cli --mcu=TEENSY41 -w -s -v "$FW/$img"
fi
echo "FLASHED $img"
