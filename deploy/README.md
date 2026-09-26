# Car system — install and bench bring-up

Five boards, one hub. Everything below is built, and everything that can run without the boards has
been run: `bash deploy/run_all_tests.sh` repeats all of it and prints one verdict.

```
  Teensy 4.1  CAN logger    ──USB──┐
  Teensy 4.1  vent display  ──USB──┼──  Orange Pi 4 Pro  (car-hub.service, http://<pi>:8090)
  ESP32-S3    climate node  ──USB──┘          │ Ethernet, 10.20.0.1  <->  10.20.0.2
                                         PZ7020 Zynq  (zynq-agent, TCP 8091, beacon UDP 8092)
```

| Unit | What you install | State | Proven by |
|---|---|---|---|
| PZ7020 Zynq | the SD card (already written) | image sha256 in `hardware/pz7020-starlite/linux/out/sd-image.sha256`; the write and read-back record is the State table of `BOOT-SD-runbook.md` | `check_image_contents.sh`: boot files and agent equal the repo's · `qemu_agent_test.sh`: the image boots and the real hub reads it over eth0 |
| Orange Pi 4 Pro | `car-bundle.tar.gz` → `install.sh` | bundle built | the real installer under systemd (WSL): service up, `/health` answers, Zynq found by its beacon in 2 s · car-LAN block run against NetworkManager 1.36 |
| CAN logger (Teensy 4.1) | `firmware/car-can-logger.ino.hex` | rebuilt from source byte-identical | its real code on the host: 14 checks (listen-only, `I`/`R`/`X`, SD log) + the Pi parsers read its output |
| Vent display (Teensy 4.1) | `firmware/vent-display.ino.hex` | rebuilt byte-identical | 9 checks, fed over both USB and the link UART |
| Climate node (ESP32-S3) | `firmware/vent-climate-node.ino.merged.bin` | rebuilt byte-identical, `CDCOnBoot=cdc` enforced by the source | 12 checks: control law, fail-safe CLOSED, KEY=value telemetry |

## 1. Zynq

The card is ready. Board setup (boot jumper, which USB-C does what) and the hands-off boot watcher:
[hardware/pz7020-starlite/BOOT-SD-runbook.md](../hardware/pz7020-starlite/BOOT-SD-runbook.md).

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File hardware\pz7020-starlite\linux\watch_boot.ps1
```

A good boot ends in the `zynq-report`: `mem_total` about 1 GB, `pl_done 1`, `eth0` holding `10.20.0.2`.
The 32-bit / 1 GB DDR setting has never run on this board (the 16-bit one reached the kernel on
2026-09-24). If the boot stops at `DDR` or right after SPL, put the card back in the PC and:

```bash
bash hardware/pz7020-starlite/linux/update_card.sh E out/fallback-512MB
```

## 2. Orange Pi

Copy `deploy/car-bundle.tar.gz` to the Pi (USB stick is fine), then on the Pi:

```bash
mkdir -p ~/car && tar -xzf car-bundle.tar.gz -C ~/car && sudo bash ~/car/deploy/orangepi/install.sh
```

It ends with `INSTALLED` and the API address. It sets up:
- `car-hub.service` (starts at boot, restarts on failure, runs unprivileged)
- udev access to the boards
- `car-lan`: the Pi's Ethernet port = `10.20.0.1/24`, the Zynq's link. If that port is the Pi's
  internet at install time, the profile is left manual and the installer prints the one command to
  bring it up once the Zynq is cabled.

## 3. Flash the three nodes

On the Pi, one board plugged in at a time:

```bash
sudo /opt/car/flash.sh logger
```
```bash
sudo /opt/car/flash.sh display
```
```bash
sudo /opt/car/flash.sh climate
```

Or from this PC: `powershell -ExecutionPolicy Bypass -File deploy\firmware\flash_windows.ps1 logger`
(`display`, `climate`). Both check the image against `SHA256SUMS` before sending a byte. The climate
node talks on the ESP32-S3's **native USB** port (the one marked USB, not COM/UART).

## 4. Cable it and look

- The three nodes: USB into the Pi, any ports, any order. The hub asks each port who it is (`ID?`).
- Pi Ethernet → the Zynq's **PS** RJ45 (`eth0`). The PL RJ45 is `eth1`, for later.
- On the Pi: `curl -s localhost:8090/health` → `healthy: true` with `car-can-logger`, `vent-display`,
  `climate-node` and `zynq` under `devices`. `curl -s localhost:8090/api` shows every value with its age. The display's status
  line reads `OK`, or `FLT <node>` naming whatever went quiet.

## Not yet proven on the real hardware

These are the first things the bench will answer; nothing here could test them.
- The 32-bit / 1 GB DDR init on this board (fallback above).
- `eth1`'s RGMII timing (TXC skew 2 ns from the PHY straps; RX delay from the PHY).
- `car-lan` on the Pi's own NetworkManager (same commands passed against NM 1.36 in WSL).
- The boards themselves: USB enumeration on the Pi, CAN bitrates on the car (bus 1 500 k, bus 2
  100 k in `config.h`), and the damper servo travel (`SERVO_US_CLOSED` / `SERVO_US_OPEN` are marked
  MEASURE in the climate node source).

## Rebuild and retest

```bash
bash deploy/run_all_tests.sh
```

Rebuilds the three firmware images and compares them with what gets flashed, rebuilds the bundle,
runs the FPGA testbenches, every Python self-test, the system test, the host firmware tests, the
contract check, the installer under systemd, and boots the Zynq image in QEMU.
