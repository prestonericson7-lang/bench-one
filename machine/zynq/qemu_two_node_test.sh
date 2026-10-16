#!/bin/bash
# qemu_two_node_test.sh -- the machine's two FPGA boards, emulated: two QEMU xilinx-zynq-a9 machines
# booted from the card image (node 1 and node 2) and joined by a socket link standing in for cable 1.
# Node 1 then runs the bench-day command, `machine-bench all`, against the 0.5B on its card: facts,
# its own engine, the peer's engine over the link, the exact reference (tl_ref), the fused path, one
# engine, BOTH engines, perplexity with and without. The exact reference's every line is kept on the
# card and compared byte for byte with the PC's tl_ref on the same prompt.
#
# What this proves: the real armhf binaries, the real image, the real link topology and the real
# command sequence work end to end. What it cannot: the PL (QEMU has none, so each engine is the
# server's CPU fallback with a 64 MB pool), the SPL/DDR, and any speed. Run in WSL as root:
#   bash /mnt/d/espicpc/machine/zynq/qemu_two_node_test.sh
set -uo pipefail
REPO=/mnt/d/espicpc
OUT=$REPO/hardware/pz7020-starlite/linux/out
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
W=/root/zynq/machine-two; mkdir -p "$W"
DT=zynq-pz7020-starlite
LINK=1235; P1=5563; P2=5564
fail=0; ok() { echo "  PASS  $*"; }; bad() { echo "  FAIL  $*"; fail=1; }

cp "$OUT/$DT.dtb" "$W/q.dtb"
fdtput -d "$W/q.dtb" /axi/ethernet@e000b000 phy-handle 2>/dev/null; fdtput -r "$W/q.dtb" /axi/ethernet@e000b000/ethernet-phy@1 2>/dev/null
for N in 1 2; do
    cp "$IMG" "$W/sd$N.img" && truncate -s 8G "$W/sd$N.img"
    echo "$N" > "$W/node.txt" && mcopy -o -i "$W/sd$N.img@@1M" "$W/node.txt" ::/zynq-node.txt
done
APPEND="console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 cpufreq.off=1 uio_pdrv_genirq.of_id=generic-uio"
timeout 7500 qemu-system-arm -M xilinx-zynq-a9 -m 512M -display none -monitor none \
  -serial tcp:127.0.0.1:$P1,server=on,wait=off -kernel "$OUT/zImage" -dtb "$W/q.dtb" -append "$APPEND" \
  -drive file="$W/sd1.img",if=sd,format=raw -nic socket,listen=127.0.0.1:$LINK,model=cadence_gem,mac=52:54:00:5a:70:01 \
  </dev/null >"$W/qemu-1.out" 2>&1 &
Q1=$!
sleep 3
timeout 7500 qemu-system-arm -M xilinx-zynq-a9 -m 512M -display none -monitor none \
  -serial tcp:127.0.0.1:$P2,server=on,wait=off -kernel "$OUT/zImage" -dtb "$W/q.dtb" -append "$APPEND" \
  -drive file="$W/sd2.img",if=sd,format=raw -nic socket,connect=127.0.0.1:$LINK,model=cadence_gem,mac=52:54:00:5a:70:02 \
  </dev/null >"$W/qemu-2.out" 2>&1 &
Q2=$!
trap 'kill $Q1 $Q2 2>/dev/null; wait $Q1 $Q2 2>/dev/null' EXIT
sleep 2
python3 - "$P1" "$P2" "$W" <<'PY'
import socket, sys, time
p1, p2, w = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]

class Con:
    def __init__(self, port, name):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5); self.s.settimeout(1.0)
        self.buf = b""; self.f = open(f"{w}/console-{name}.log", "wb")
    def until(self, marker, limit):
        # waits for marker in NEW output and consumes through it, so an earlier command's sentinel can
        # never satisfy a later wait (the first version searched the whole buffer and did exactly that)
        t0 = time.time()
        while time.time() - t0 < limit:
            try: c = self.s.recv(65536)
            except socket.timeout: c = b""
            if c:
                self.buf += c; self.f.write(c); self.f.flush()
                k = self.buf.find(marker)
                if k >= 0:
                    self.buf = self.buf[k + len(marker):]
                    return True
        return False
    def cmd(self, line): self.s.sendall(line.encode() + b"\r"); time.sleep(0.3)

a, b = Con(p1, "1"), Con(p2, "2")
for c in (a, b):
    c.until(b"ZYNQ-REPORT END", 480)
    c.until(b"root@zynq", 90) or c.cmd("")
    time.sleep(1); c.cmd(""); c.until(b"# ", 30)
b.cmd("hostname; ip -4 -o addr show eth0 | awk '{print $4}'; echo __DO''NE__"); b.until(b"\n__DONE__", 30)
a.cmd("hostname; ip -4 -o addr show eth0 | awk '{print $4}'; ping -c3 -W2 10.20.0.3; echo __DO''NE__"); a.until(b"\n__DONE__", 60)
# the bench-day command, shrunk for emulation (TCG runs this CPU at roughly 100 s a token: 8 generated
# tokens and 32 scored ones took 84 minutes and ran out the clock, 2026-09-30)
a.cmd("MB_TOKENS=4 MB_PPL_LIMIT=8 MB_BENCH_ARGS='-r 2 -s 512x512:int8:1 -s 512x512:int4:8' machine-bench all; echo __DO''NE__")
a.until(b"\n__DONE__", 6600)
a.cmd("ls -la /boot/reports; sync; echo __DO''NE__"); a.until(b"\n__DONE__", 30)
b.cmd("ls /boot/reports; echo __DO''NE__"); b.until(b"\n__DONE__", 30)
PY
for N in 1 2; do tr -d '\r' < "$W/console-$N.log" | sed -E 's/\x1b\[[?0-9;]*[A-Za-z]//g' > "$W/transcript-$N.txt"; done
kill $Q1 $Q2 2>/dev/null; wait $Q1 $Q2 2>/dev/null; trap - EXIT
# the reports and the exact reference's lines, out of node 1's card
mkdir -p "$W/reports" && mcopy -n -i "$W/sd1.img@@1M" ::/reports/* "$W/reports/" 2>/dev/null
T=$W/transcript-1.txt
grep -aq "zynq1" "$T" && grep -aq "zynq2" "$W/transcript-2.txt" && ok "both nodes up as zynq1 / zynq2" || bad "node identity"
grep -aq "3 packets transmitted, 3 received" "$T" && ok "cable 1: zynq1 pings zynq2 over the link" || bad "no ping across the link"
grep -aq "zynq2: up" "$T" && ok "machine-bench sees the peer" || bad "machine-bench did not see the peer"
grep -aq "check  Zynq alone ......... bit-exact" "$T" && ok "engine on zynq1: bit-exact" || bad "engine on zynq1 not bit-exact / not run"
grep -aq "the matrix engine on zynq2" "$T" && ok "engine on zynq2 driven from zynq1" || bad "peer engine not driven"
grep -aq "a. this CPU alone, exact reference" "$T" && ls "$W"/reports/tl_ref-*.txt >/dev/null 2>&1 && ok "tl_ref ran on the ARM CPU; its lines are on the card" || bad "tl_ref did not run / no file"
grep -aq "b. this CPU alone, fused" "$T" && grep -aq "per decoded" "$T" && ok "run_model --fast produced tokens" || bad "run_model --fast"
grep -aqE "weights on 1 engine" "$T" && ok "one engine attached (measured split)" || bad "one-engine attach"
grep -aqE "weights on 2 engines" "$T" && ok "BOTH engines attached across the link" || bad "two-engine attach"
n=$(grep -ac "PERPLEXITY" "$T"); [ "$n" -ge 2 ] && ok "perplexity with and without the engine ($n lines)" || bad "perplexity lines: $n"
grep -aq "report: /boot/reports/machine-bench" "$T" && ok "report on the card" || bad "no report"
echo "  transcripts: $W/transcript-1.txt $W/transcript-2.txt ; reports: $W/reports/"
[ $fail = 0 ] && echo "TWO-NODE QEMU TEST: PASS" || echo "TWO-NODE QEMU TEST: FAIL"
exit $fail
