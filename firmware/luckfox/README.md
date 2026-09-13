# Luckfox packet hub

Gives the Teensy↔Luckfox UART a job: the Luckfox Pico A reads the line protocol the
Teensy already streams, **logs every packet to the microSD**, and serves a **live
green-on-black web console** over its USB-gadget ethernet. You can also send commands
back to the Teensy (mode / freq / preset / power / sync / send / stop) from the page.

Stdlib Python 3 only — the stock image has `python3` but no `pyserial`.

## Wiring (already in WIRING.md)
| Teensy | Luckfox | note |
|--------|---------|------|
| pin 8 (TX2) → | UART3_RX | 100 Ω in series |
| pin 7 (RX2) ← | UART3_TX | 100 Ω in series |
| GND | GND | common ground |

Both run at 1 Mbaud. The Luckfox has its own USB power + microSD.

## Deploy (SD-card copy — the easy way, no adb)
1. Put the microSD in a card reader on the PC and copy `packet_hub.py` onto it
   (e.g. into a `lora/` folder), and copy `S99packethub` too if you want autostart.
2. Boot the Luckfox with the card in. Then over its serial/adb shell:
   ```sh
   mount -t vfat /dev/mmcblk1p1 /mnt/sdcard        # if not already mounted
   cp /mnt/sdcard/lora/packet_hub.py /root/
   python3 /root/packet_hub.py
   ```
   You should see `serial /dev/ttyS3 @ 1000000 open` and
   `web console on http://<luckfox-ip>:8080`.

If `/dev/ttyS3` is not present, UART3 isn't muxed yet — run `luckfox-config`
(Peripherals → UART3) or enable it in the device tree, reboot, and retry.

## Open the console
The Luckfox USB-gadget ethernet is the board at **172.32.0.93** by default
(check with `ifconfig usb0`). From the PC on the same USB link, open:
```
http://172.32.0.93:8080
```
Status line, live packet table, sweep bars, and control buttons. Captures are written to
`/mnt/sdcard/lora/cap_<epoch>.csv` (host_epoch,freq,rssi,snr,len,hex).

## Autostart (appliance mode)
Copy `S99packethub` to `/etc/init.d/`, make it executable, reboot:
```sh
cp /mnt/sdcard/lora/S99packethub /etc/init.d/ && chmod +x /etc/init.d/S99packethub
```
It mounts the SD and launches the hub at boot.

## Env overrides
`HUB_DEV` (default /dev/ttyS3), `HUB_BAUD` (1000000), `HUB_PORT` (8080),
`HUB_LOGDIR` (/mnt/sdcard/lora), `HUB_SDDEV` (/dev/mmcblk1p1), `HUB_SDMOUNT` (/mnt/sdcard).

## Notes
- 64 MB RAM, ~15 MB free: the ring buffer is capped at 400 packets for the web view;
  the SD log keeps everything.
- The web console runs even if the serial port can't open — it shows the error in the
  status bar so you can tell the hub is alive.
- Tested on the host with a simulated Teensy stream (parser, SD logging, ring buffer,
  sweep detection, web page, `/api`, `/cmd`). Only the termios serial line is board-only.
