#!/bin/bash
# qemu_agent_test.sh -- boot the real SD image in QEMU (xilinx-zynq-a9) with the board's TCP 8091
# forwarded to the host, then point the real hub.py at it: proves the image's networkd config, the
# agent service and the hub <-> Zynq protocol together. Run inside WSL as root.
set -uo pipefail
REPO=/mnt/d/espicpc
OUT=$REPO/hardware/pz7020-starlite/linux/out
IMG=/root/zynq/pz7020-starlite-sd.img
LOG=/root/zynq/qemu-agent.log
DT=zynq-pz7020-starlite
cp "$IMG" /root/zynq/qemu-agent-sd.img && truncate -s 2G /root/zynq/qemu-agent-sd.img
# QEMU's GEM model does not put its PHY at MDIO address 1 like the board's RTL8211F: give the emulated
# run a DTB copy without the fixed phy-handle so macb scans the bus. The board's DTB is untouched.
QDTB=/root/zynq/qemu-agent.dtb; cp "$OUT/$DT.dtb" "$QDTB"
fdtput -d "$QDTB" /axi/ethernet@e000b000 phy-handle && fdtput -r "$QDTB" /axi/ethernet@e000b000/ethernet-phy@1 \
  && echo "emulation DTB: gem0 phy-handle removed (PHY scanned)"
timeout 300 qemu-system-arm -M xilinx-zynq-a9 -m 1024M -nographic -serial mon:stdio \
  -kernel "$OUT/zImage" -dtb "$QDTB" \
  -append "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 cpufreq.off=1" \
  -drive file=/root/zynq/qemu-agent-sd.img,if=sd,format=raw \
  -nic user,model=cadence_gem,hostfwd=tcp:127.0.0.1:18091-:8091 </dev/null >"$LOG" 2>&1 &
QPID=$!
python3 "$REPO/firmware/telemetry-hub/hub.py" --no-scan --port 8095 --beacon-port 18092 \
  --zynq 127.0.0.1:18091 >/tmp/hub-q.log 2>&1 &
HPID=$!
ok=0
for i in $(seq 1 90); do
  sleep 3
  r=$(python3 - <<'PY'
import json, urllib.request, urllib.error
try: b = urllib.request.urlopen("http://127.0.0.1:8095/api", timeout=3).read()
except urllib.error.HTTPError as e: b = e.read()
except Exception: print(""); raise SystemExit
j = json.loads(b); v = j["values"]
print(j["devices"].get("zynq", ""), (v.get("ZYNQ_UP") or {}).get("value", ""), (v.get("ZYNQ_MEM") or {}).get("value", ""),
      (v.get("PL_STATE") or {}).get("value", ""))
PY
)
  set -- $r
  if [ -n "${2:-}" ]; then echo "after $((i*3)) s the hub is reading the emulated board: zynq=$1 ZYNQ_UP=$2 ZYNQ_MEM=${3}MB PL_STATE=${4:-?}"; ok=1; break; fi
done
sleep 12   # let zynq-report (8 s after network) reach the console
kill $HPID 2>/dev/null; kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
echo "---- guest log ----"
grep -a -n -E "Memory:|systemd-networkd|zynq-agent|Started .*[Nn]etwork|login:|ZYNQ-REPORT|^(eth|ip|addr|mem_total|pl_done|net)|10\.20\.0\.2|10\.0\.2\.15|Kernel panic|FAILED" "$LOG" | cut -c1-150 | head -40
[ $ok = 1 ] && echo "QEMU AGENT TEST: PASS" || { echo "QEMU AGENT TEST: FAIL"; tail -20 /tmp/hub-q.log; }
exit $((1 - ok))
