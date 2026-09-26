#!/bin/bash
# qemu_accel_test.sh -- boot the real SD image in QEMU (xilinx-zynq-a9) and drive every accelerator
# service the Orange Pi uses, from this machine standing in for the Pi, through forwarded ports:
#   zaccel-server  8093  -> 127.0.0.1:18093   the matrix engine (QEMU has no PL: CPU engine answers)
#   fpgagpud       7777  -> 127.0.0.1:17777   the GPU daemon (no PL: it must say so, not hang)
#   nbd-server    10809  -> 127.0.0.1:20809   the Zynq's DDR3 exported as the Pi's swap
# and check the board's own boot report for the services, the UIO windows and the PL-owned DDR3.
# Run inside WSL as root.   Exit 0 = every check passed.
set -uo pipefail
REPO=/mnt/d/espicpc
OUT=$REPO/hardware/pz7020-starlite/linux/out
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
W=/root/zynq/accel-test; mkdir -p "$W"
LOG=$W/console.log
DT=zynq-pz7020-starlite
fail=0; ok() { echo "  PASS  $*"; }; bad() { echo "  FAIL  $*"; fail=1; }

cp "$IMG" "$W/sd.img" && truncate -s 2G "$W/sd.img"
# QEMU's GEM PHY is not at MDIO address 1: drop the fixed phy-handle in an emulation-only copy
cp "$OUT/$DT.dtb" "$W/q.dtb"
fdtput -d "$W/q.dtb" /axi/ethernet@e000b000 phy-handle && fdtput -r "$W/q.dtb" /axi/ethernet@e000b000/ethernet-phy@1
timeout 600 qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -serial mon:stdio \
  -kernel "$OUT/zImage" -dtb "$W/q.dtb" \
  -append "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 cpufreq.off=1 uio_pdrv_genirq.of_id=generic-uio" \
  -drive file="$W/sd.img",if=sd,format=raw \
  -nic user,model=cadence_gem,hostfwd=tcp:127.0.0.1:18093-:8093,hostfwd=tcp:127.0.0.1:17777-:7777,hostfwd=tcp:127.0.0.1:20809-:10809 \
  </dev/null >"$LOG" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null; wait $QPID 2>/dev/null' EXIT

echo "== waiting for the board's boot report"
for i in $(seq 1 120); do grep -aq "ZYNQ-REPORT END" "$LOG" && break; sleep 3; done
grep -aq "ZYNQ-REPORT END" "$LOG" || { bad "no boot report within 360 s"; tail -30 "$LOG"; exit 1; }
# the report reaches the console through the journal: each line carries a "[ t] python3[pid]: " prefix
rep=$(sed -n '/ZYNQ-REPORT BEGIN/,/ZYNQ-REPORT END/p' "$LOG" | tr -d '\r' | sed -E 's/^.*python3\[[0-9]+\]: //')
echo "$rep" | grep -aE "^(mem_total|pl_done|failed_units|accel|uio|pl_ddr|zynqram|eth0)" | sed 's/^/    /'
echo "$rep" | grep -aq "zaccel-server active" && ok "zaccel-server running" || bad "zaccel-server not active"
echo "$rep" | grep -aq "fpgagpud active" && ok "fpgagpud running" || bad "fpgagpud not active"
echo "$rep" | grep -aq "nbd-server active" && ok "nbd-server running" || bad "nbd-server not active"
for n in pl_regs@40000000@0x40000000 zaccel-dma@40400000@0x40400000 zaccel-mem@10000000@0x10000000; do
  echo "$rep" | grep -aq "$n" && ok "UIO $n" || bad "UIO $n missing"
done
echo "$rep" | grep -aE "^pl_ddr" | grep -aq "00000000-0fffffff : System RAM" && ok "Linux RAM is 0-256 MB; the engine (0x10000000) and GPU (0x1E000000) windows are the PL's" || bad "System RAM does not exclude the PL DDR3"
echo "$rep" | grep -aq "^failed_units *none" && ok "no failed units" || bad "failed units: $(echo "$rep" | grep -a '^failed_units')"

echo "== matrix engine over the network (the Pi's view)"
python3 - <<'PY' || fail=1
import random, socket, struct, sys, time
H, P = "127.0.0.1", 18093
def conn():
    for _ in range(60):
        try: return socket.create_connection((H, P), timeout=10)
        except OSError: time.sleep(2)
    raise SystemExit("  FAIL  no zaccel-server on the forwarded port")
s = conn(); seq = 0
def rpc(op, payload=b""):
    global seq; seq += 1
    s.sendall(struct.pack("<4I", 0x3151415A, op, seq, len(payload)) + payload)
    hdr = b""
    while len(hdr) < 16: hdr += s.recv(16 - len(hdr))
    magic, st, sq, n = struct.unpack("<4I", hdr)
    body = b""
    while len(body) < n: body += s.recv(n - len(body))
    assert magic == 0x3152415A and sq == seq, "bad reply header"
    return st, body
st, b = rpc(1)
info = struct.unpack("<7I", b[:28])
print(f"    INFO version {info[0]} engine {'pl' if info[1] else 'cpu'} mem {info[3]}/{info[2]} MB max_cols {info[4]} max_batch {info[5]} selftest {info[6]}")
ok = st == 0 and info[6] == 0
st, b = rpc(5, b"ping-from-pi")
ok &= (st == 0 and b == b"ping-from-pi")
rng = random.Random(7)
for mode, rows, cols, nb in ((0, 64, 257, 3), (1, 33, 100, 8), (0, 512, 4096, 1)):
    lo, hi = (-8, 7) if mode == 0 else (-128, 127)
    W = [[rng.randint(lo, hi) for _ in range(cols)] for _ in range(rows)]
    A = [[rng.randint(-128, 127) for _ in range(cols)] for _ in range(nb)]
    # SPEC §4 LOAD: each row packed per §1 and padded to whole 8-byte beats
    raw = b""
    for r in W:
        if mode == 0:
            nib = [w & 15 for w in r] + [0] * (-cols % 16)
            row = bytes(nib[i] | (nib[i + 1] << 4) for i in range(0, len(nib), 2))
        else:
            row = bytes(w & 255 for w in r) + bytes(-cols % 8)
        raw += row
    st, b = rpc(2, struct.pack("<3I", mode, rows, cols) + raw)
    if st != 0: print(f"  FAIL  LOAD status {st}"); ok = False; continue
    tid = struct.unpack("<I", b[:4])[0]
    t0 = time.time()
    st, b = rpc(4, struct.pack("<2I", tid, nb) + b"".join(bytes(a & 255 for a in v) for v in A))
    dt = time.time() - t0
    cyc, eng = struct.unpack("<2I", b[:8]); Y = struct.unpack(f"<{rows * nb}i", b[8:])
    want = [sum(W[r][k] * A[v][k] for k in range(cols)) for r in range(rows) for v in range(nb)]
    good = st == 0 and list(Y) == want
    print(f"    GEMV mode {mode} {rows}x{cols} nb {nb}: {'exact' if good else 'WRONG'} (engine {'pl' if eng else 'cpu'}, {dt*1000:.0f} ms incl. network, emulated CPU)")
    ok &= good
    rpc(3, struct.pack("<I", tid))
st, b = rpc(4, struct.pack("<2I", 999999, 1) + b"\0")
ok &= (st == 4)
print("  PASS  matrix engine: INFO, PING, LOAD/GEMV/FREE exact, unknown tensor rejected" if ok else "  FAIL  matrix engine")
sys.exit(0 if ok else 1)
PY

echo "== the Pi's own client and benchmark against the board (answers checked; emulated timings do not count)"
if [ -x "$REPO/accel/pi/out/host/zaccel-bench" ]; then
  timeout 300 "$REPO/accel/pi/out/host/zaccel-bench" -H 127.0.0.1 -p 18093 -r 1 -s 256x1024:int4:8 -s 128x300:int8:3 \
    >"$W/bench.txt" 2>&1; rc=$?
  grep -aE "wrong|exact|RESULT|NOT REACHABLE" "$W/bench.txt" | head -8 | sed 's/^/    /'
  [ $rc = 0 ] && ok "zaccel-bench: every answer from the board exact" || bad "zaccel-bench exit $rc (tail: $(tail -2 "$W/bench.txt"))"
else
  bad "accel/pi/out/host/zaccel-bench not built"
fi

echo "== GPU daemon (no PL under QEMU: it must answer and report that)"
if [ -x "$REPO/accel/gpu/pi/build/x86/gpu_stat" ]; then
  timeout 30 "$REPO/accel/gpu/pi/build/x86/gpu_stat" --fpga 127.0.0.1:17777 --teensy none >"$W/gpu_stat.txt" 2>&1; rc=$?
  sed 's/^/    /' "$W/gpu_stat.txt" | head -12
  [ $rc -ne 124 ] && ok "fpgagpud answered gpu_stat (rc $rc)" || bad "gpu_stat hung"
else
  python3 -c "import socket; socket.create_connection(('127.0.0.1',17777),timeout=10).close()" \
    && ok "fpgagpud accepts connections on 7777" || bad "fpgagpud not reachable"
fi

echo "== the Zynq's RAM as the Pi's block device"
modprobe nbd max_part=0 2>/dev/null
DEV=/dev/nbd7
if command -v nbd-client >/dev/null; then
  nbd-client -d $DEV >/dev/null 2>&1
  if timeout 60 nbd-client 127.0.0.1 20809 $DEV -N zynqram >"$W/nbd.txt" 2>&1; then
    sz=$(blockdev --getsize64 $DEV)
    head -c 8388608 /dev/urandom > "$W/pat.bin"
    dd if="$W/pat.bin" of=$DEV bs=1M oflag=direct status=none && sync
    dd if=$DEV of="$W/back.bin" bs=1M count=8 iflag=direct status=none
    cmp -s "$W/pat.bin" "$W/back.bin" && ok "zynqram export: $((sz / 1048576)) MB, 8 MB written and read back identical" || bad "zynqram read-back differs"
    nbd-client -d $DEV >/dev/null 2>&1
  else
    bad "nbd-client could not attach zynqram: $(tail -2 "$W/nbd.txt")"
  fi
else
  bad "nbd-client not installed on this test host"
fi

echo
[ $fail = 0 ] && echo "QEMU ACCEL TEST: PASS" || echo "QEMU ACCEL TEST: FAIL"
exit $fail
