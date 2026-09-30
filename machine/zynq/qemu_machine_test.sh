#!/bin/bash
# qemu_machine_test.sh -- boot the machine's card image in QEMU (xilinx-zynq-a9) and prove, on the real
# root filesystem, what the boards will do at their first boot: the identity script gives the board its
# name and address from /boot/zynq-node.txt (both values tried), the models are on the card, the
# self-test runs, the static tools execute on this glibc. QEMU has no PL, no SPL and no DDR PHY -- those
# are the board's own first test. Run in WSL as root:  bash /mnt/d/espicpc/machine/zynq/qemu_machine_test.sh
set -uo pipefail
REPO=/mnt/d/espicpc
OUT=$REPO/hardware/pz7020-starlite/linux/out
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
W=/root/zynq/machine-test; mkdir -p "$W"
DT=zynq-pz7020-starlite
PORT=5561
fail=0; ok() { echo "  PASS  $*"; }; bad() { echo "  FAIL  $*"; fail=1; }

cp "$OUT/$DT.dtb" "$W/q.dtb"
fdtput -d "$W/q.dtb" /axi/ethernet@e000b000 phy-handle 2>/dev/null; fdtput -r "$W/q.dtb" /axi/ethernet@e000b000/ethernet-phy@1 2>/dev/null

run_node() {   # run_node N  -> boots a copy with zynq-node.txt = N, drives the console, prints the transcript
    local N=$1 LOG=$W/console-$N.log
    cp "$IMG" "$W/sd.img" && truncate -s 8G "$W/sd.img"
    echo "$N" > "$W/node.txt" && mcopy -o -i "$W/sd.img@@1M" "$W/node.txt" ::/zynq-node.txt
    timeout 900 qemu-system-arm -M xilinx-zynq-a9 -m 512M -display none -monitor none \
      -serial tcp:127.0.0.1:$PORT,server=on,wait=off \
      -kernel "$OUT/zImage" -dtb "$W/q.dtb" \
      -append "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 cpufreq.off=1 uio_pdrv_genirq.of_id=generic-uio" \
      -drive file="$W/sd.img",if=sd,format=raw -nic user,model=cadence_gem </dev/null >"$W/qemu-$N.out" 2>&1 &
    local QPID=$!
    sleep 2
    python3 - "$PORT" "$LOG" <<'PY'
import socket, sys, time
port, log = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.settimeout(1.0)
buf = b""; f = open(log, "wb")
def read_until(marker, limit):
    # waits for marker in NEW output and consumes through it (an earlier sentinel must not satisfy a later wait)
    global buf
    t0 = time.time()
    while time.time() - t0 < limit:
        try:
            c = s.recv(4096)
        except socket.timeout:
            c = b""
        if c:
            buf += c; f.write(c); f.flush()
            k = buf.find(marker)
            if k >= 0:
                buf = buf[k + len(marker):]
                return True
    return False
# autologin gives a root shell on ttyPS0; the report prints first
read_until(b"ZYNQ-REPORT END", 420)
read_until(b"root@zynq", 60) or s.sendall(b"\r")
time.sleep(1); s.sendall(b"\r"); read_until(b"# ", 20)
cmds = [b"hostname", b"ip -4 -o addr show eth0 | awk '{print $4}'", b"cat /proc/sys/kernel/hostname",
        b"ls -la /opt/machine/models /usr/local/lib/machine", b"/usr/local/lib/machine/run_model 2>&1 | head -2",
        b"machine-bench facts", b"echo __DO''NE__"]       # the echo of the typed line must not match the sentinel
for c in cmds:
    s.sendall(c + b"\r"); time.sleep(0.3)
read_until(b"\n__DONE__", 300)
f.close()
PY
    kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
    # strip CR and every CSI sequence, including bracketed-paste ESC[?2004h/l which precedes each output line
    tr -d '\r' < "$LOG" | sed -E 's/\x1b\[[?0-9;]*[A-Za-z]//g' > "$W/transcript-$N.txt"
}

for N in 1 2; do
    echo "== node $N"
    run_node $N
    T=$W/transcript-$N.txt
    grep -aq "ZYNQ-REPORT END" "$T" && ok "node $N: booted to the report" || bad "node $N: no boot report"
    grep -aq "zynq-node: node $N -> zynq$N" "$T" && ok "node $N: zynq-node ran (kernel log)" || bad "node $N: zynq-node did not report"
    grep -aqx "zynq$N" "$T" && ok "node $N: hostname zynq$N" || bad "node $N: hostname not zynq$N"
    grep -aq "10.20.0.$((1 + N))/24" "$T" && ok "node $N: eth0 10.20.0.$((1 + N))" || bad "node $N: address not 10.20.0.$((1 + N))"
    if [ $N = 1 ]; then
        grep -aq "qwen3b.gguf" "$T" && ok "3B model on the card" || bad "3B model missing"
        grep -aq "qwen05b.gguf" "$T" && ok "0.5B model on the card" || bad "0.5B model missing"
        grep -aq "run_model <model.gguf>" "$T" && ok "the static armhf run_model executes on this rootfs" || bad "run_model did not execute"
        grep -aq "peers over cable 1" "$T" && ok "machine-bench facts ran" || bad "machine-bench facts did not run"
        grep -aq "report: /boot/reports/machine-bench" "$T" && ok "report written to /boot/reports" || bad "no report on the card"
    fi
    echo "  transcript: $T"
done
[ $fail = 0 ] && echo "MACHINE QEMU TEST: PASS" || echo "MACHINE QEMU TEST: FAIL"
exit $fail
