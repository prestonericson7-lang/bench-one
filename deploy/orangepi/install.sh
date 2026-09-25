#!/bin/bash
# install.sh -- put the car hub on the Orange Pi 4 Pro (official Ubuntu image). Run once, as root:
#
#     sudo bash install.sh            # from the unpacked car-bundle directory
#
# What it does (idempotent -- run it again after any update):
#   * apt: python3-serial (hub), teensy-loader-cli + esptool (flash.sh). Offline is fine if they are
#     already there; missing ones are reported, not fatal.
#   * /opt/car            the hub, the CAN tools, the voice-assistant code, the firmware images
#   * /etc/udev/rules.d   Teensy + ESP32 access for the 'dialout' group, and ModemManager told to
#                         keep its hands off them (it probes ttyACM ports with AT commands otherwise)
#   * car-lan             NetworkManager profile: the wired port = 10.20.0.1/24, the Zynq's link
#   * car-hub.service     starts at boot, restarts on failure, runs unprivileged (DynamicUser +
#                         dialout), serves http://<pi>:8090/api
#   * checks: the hub's own selftest, the service running, /health answering
set -uo pipefail
SRC=$(cd "$(dirname "$0")/../.." && pwd)       # the bundle root (this file is <root>/deploy/orangepi/)
[ -f "$SRC/firmware/telemetry-hub/hub.py" ] || { echo "bundle incomplete: no $SRC/firmware/telemetry-hub/hub.py"; exit 1; }
DEST=/opt/car
[ "$(id -u)" = 0 ] || { echo "run as root: sudo bash $0"; exit 1; }

say() { printf '\n== %s\n' "$*"; }
say "packages"
export DEBIAN_FRONTEND=noninteractive
if apt-get install -y -q python3 python3-serial teensy-loader-cli esptool >/tmp/car-apt.log 2>&1; then
  echo "python3-serial, teensy-loader-cli, esptool installed"
else
  echo "apt-get failed (offline?) -- see /tmp/car-apt.log. Checking what is already present:"
fi
python3 -c "import serial" 2>/dev/null && echo "  pyserial: ok" || { echo "  pyserial: MISSING (the hub needs it)"; MISSING=1; }
command -v teensy_loader_cli >/dev/null && echo "  teensy_loader_cli: ok" || echo "  teensy_loader_cli: missing (only flash.sh needs it)"
command -v esptool >/dev/null || command -v esptool.py >/dev/null && echo "  esptool: ok" || echo "  esptool: missing (only flash.sh needs it)"
[ "${MISSING:-0}" = 1 ] && { echo "cannot continue without pyserial"; exit 2; }

say "files -> $DEST"
mkdir -p "$DEST/telemetry-hub" "$DEST/car-can-logger" "$DEST/voice-assistant" "$DEST/firmware"
cp "$SRC/firmware/telemetry-hub/"{hub.py,can_bridge.py,system_test.py} "$DEST/telemetry-hub/" || exit 3
# the signal map is user data: never overwrite one that is already there
[ -f "$DEST/telemetry-hub/signals.json" ] || cp "$SRC/firmware/telemetry-hub/signals.json" "$DEST/telemetry-hub/"
cp "$SRC/firmware/car-can-logger/canlog.py" "$DEST/car-can-logger/"
cp "$SRC/firmware/voice-assistant/"*.py "$DEST/voice-assistant/"
cp "$SRC/deploy/firmware/"* "$DEST/firmware/" || exit 3
cp "$SRC/deploy/orangepi/flash.sh" "$DEST/flash.sh" && chmod +x "$DEST/flash.sh" || exit 3
sed -i 's/\r$//' "$DEST"/telemetry-hub/*.py "$DEST/flash.sh"
ls -la "$DEST/firmware"

say "udev rules"
install -m 0644 "$SRC/deploy/orangepi/49-car-usb.rules" /etc/udev/rules.d/49-car-usb.rules || exit 4
sed -i 's/\r$//' /etc/udev/rules.d/49-car-usb.rules
udevadm control --reload-rules 2>/dev/null && udevadm trigger 2>/dev/null
echo "installed /etc/udev/rules.d/49-car-usb.rules"

say "car LAN (the Ethernet cable to the Zynq)"
# The Zynq answers at 10.20.0.2 and broadcasts its beacon on whatever link it is on, but a direct cable
# has no DHCP server, so the Pi needs its own address there: a NetworkManager profile giving the wired
# port 10.20.0.1/24. never-default keeps the Pi's internet route on Wi-Fi. If that port is carrying
# the Pi's internet right now, the profile is created but left manual, so this never cuts a working
# connection. Skip with CAR_LAN=0; choose the port with CAR_LAN_DEV=<interface>.
if [ "${CAR_LAN:-1}" = 0 ]; then
  echo "skipped (CAR_LAN=0)"
elif ! command -v nmcli >/dev/null || ! nmcli -t general status >/dev/null 2>&1; then
  echo "NetworkManager is not running here: give the port facing the Zynq 10.20.0.1/24 by hand"
else
  DEV=${CAR_LAN_DEV:-$(nmcli -t -f DEVICE,TYPE device | awk -F: '$2 == "ethernet" { print $1; exit }')}
  if [ -z "$DEV" ]; then
    echo "no Ethernet port found -- rerun with CAR_LAN_DEV=<interface>"
  else
    AUTO=yes; [ -n "$(ip -4 route show default dev "$DEV" 2>/dev/null)" ] && AUTO=no
    nmcli connection delete car-lan >/dev/null 2>&1
    if nmcli connection add type ethernet ifname "$DEV" con-name car-lan autoconnect "$AUTO" \
         connection.autoconnect-priority 100 ipv4.method manual ipv4.addresses 10.20.0.1/24 \
         ipv4.never-default yes ipv6.method link-local >/dev/null; then
      if [ "$AUTO" = yes ]; then
        echo "car-lan: $DEV = 10.20.0.1/24 (the Zynq is 10.20.0.2), automatic from the next boot or cable plug"
      else
        echo "car-lan: $DEV carries this Pi's internet right now, so the profile is MANUAL."
        echo "  once $DEV is cabled to the Zynq:  sudo nmcli connection up car-lan"
      fi
      echo "  to give the port back to a home network:  sudo nmcli connection delete car-lan"
    else
      echo "car-lan: nmcli could not create the profile -- give $DEV 10.20.0.1/24 by hand"
    fi
  fi
fi

say "service"
install -m 0644 "$SRC/deploy/orangepi/car-hub.service" /etc/systemd/system/car-hub.service || exit 5
sed -i 's/\r$//' /etc/systemd/system/car-hub.service
systemctl daemon-reload
systemctl enable car-hub.service >/dev/null 2>&1
systemctl restart car-hub.service

say "checks"
fail=0
python3 "$DEST/telemetry-hub/hub.py" --selftest >/tmp/car-selftest.log 2>&1 \
  && echo "hub selftest: PASSED" || { echo "hub selftest: FAILED (/tmp/car-selftest.log)"; fail=1; }
sleep 3
systemctl is-active --quiet car-hub.service && echo "car-hub.service: running" \
  || { echo "car-hub.service: NOT running"; journalctl -u car-hub.service -n 20 --no-pager; fail=1; }
python3 - <<'PY' || fail=1
import json, urllib.request, urllib.error
import sys, time
body = None
for _ in range(10):
    try:
        body = urllib.request.urlopen("http://127.0.0.1:8090/health", timeout=5).read(); break
    except urllib.error.HTTPError as e:      # 503 = answering, with faults (nodes not plugged in yet)
        body = e.read(); break
    except OSError:
        time.sleep(1)
if body is None:
    print("hub /health: no answer on :8090"); sys.exit(1)
h = json.loads(body)
print("hub /health answers: healthy=%s devices=%s" % (h["healthy"], h["devices"]))
for k, v in sorted(h["faults"].items()):
    print("  waiting for %-15s %s" % (k, v))
PY
IP=$(hostname -I 2>/dev/null | awk '{print $1}')
if [ $fail = 0 ]; then
  echo; echo "INSTALLED. API: http://${IP:-<pi>}:8090/api   health: http://${IP:-<pi>}:8090/health"
  echo "Plug the Teensys / ESP32 into any USB port and power the Zynq: the hub finds them by itself."
else
  echo; echo "INSTALL FINISHED WITH ERRORS (see above)"; exit 3
fi
