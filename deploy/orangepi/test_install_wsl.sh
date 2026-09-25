#!/bin/bash
# test_install_wsl.sh -- run the real installer on Ubuntu under systemd (WSL), then prove the service
# finds a Zynq by its broadcast beacon on a real Linux network stack. Run inside WSL as root.
set -uo pipefail
B=/mnt/d/espicpc/deploy/car-bundle.tar.gz
rm -rf /tmp/car-bundle && mkdir -p /tmp/car-bundle && tar -xzf "$B" -C /tmp/car-bundle
bash /tmp/car-bundle/deploy/orangepi/install.sh; rc=$?
echo "install.sh exit: $rc"
echo "--- unit hardening as applied by systemd:"
systemctl show car-hub.service -p User -p DynamicUser -p SupplementaryGroups -p ProtectSystem -p ActiveState -p SubState
echo "--- stand-in Zynq: the real zynq_agent.py, broadcasting its beacon on this machine's LAN"
python3 /mnt/d/espicpc/firmware/telemetry-hub/zynq_agent.py --port 8091 --period 0.5 >/tmp/agent.log 2>&1 &
AG=$!
for i in $(seq 1 30); do
  sleep 1
  s=$(python3 -c "
import json,urllib.request,urllib.error
try: b=urllib.request.urlopen('http://127.0.0.1:8090/api',timeout=3).read()
except urllib.error.HTTPError as e: b=e.read()
j=json.loads(b); print(j['devices'].get('zynq',''), (j['values'].get('ZYNQ_UP') or {}).get('value',''))")
  set -- $s
  if [ -n "${2:-}" ]; then echo "hub found the Zynq at ${1} by its beacon after ${i}s; ZYNQ_UP=${2}"; break; fi
done
[ -n "${2:-}" ] || echo "FAIL: hub never connected to the agent (devices: $s)"
kill $AG 2>/dev/null
journalctl -u car-hub.service -n 3 --no-pager
[ "$rc" = 0 ] && [ -n "${2:-}" ] && { echo "INSTALL TEST: PASS"; exit 0; }
echo "INSTALL TEST: FAIL"; exit 1
