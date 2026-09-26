#!/bin/bash
# test_fallback.sh -- with no host given, the C and Python clients reach a Zynq that answers ONLY on
# 10.77.0.2 (a Pi with just the GPU's direct-cable address). WSL, root. Leaves the network as it was.
set -u
H=$(cd "$(dirname "$0")" && pwd)
ip addr add 10.77.0.2/32 dev lo 2>/dev/null; added=$?
python3 "$H/mock_server.py" --bind 10.77.0.2 --port 8093 >/tmp/fb_mock.log 2>&1 &
M=$!; sleep 1
rc=0
unset ZACCEL_HOST
if timeout 30 python3 "$H/zaccel.py" 2>&1 | tee /tmp/fb_py.txt | grep -q "10.77.0.2"; then echo "PASS  zaccel.py fell back to 10.77.0.2"; else echo "FAIL  zaccel.py: $(cat /tmp/fb_py.txt)"; rc=1; fi
if timeout 60 "$H/out/host/zaccel-bench" -r 1 -s 64x64:int8:1 >/tmp/fb_c.txt 2>&1; then echo "PASS  zaccel-bench (libzaccel) reached the Zynq with no -H"; else echo "FAIL  zaccel-bench: $(tail -3 /tmp/fb_c.txt)"; rc=1; fi
kill $M 2>/dev/null
[ $added = 0 ] && ip addr del 10.77.0.2/32 dev lo
exit $rc
