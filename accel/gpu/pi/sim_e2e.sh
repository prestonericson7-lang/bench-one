#!/usr/bin/env bash
# pi/sim_e2e.sh -- end-to-end run of the Orange Pi library + tools against the simulators (SPEC 12).
# Runs in WSL/Linux (gcc, make, python3; optional: aarch64 qemu-user binfmt, an X server in $DISPLAY).
#
#   bash pi/sim_e2e.sh              (from the project root, or: wsl.exe -d Ubuntu-22.04 -- bash -s < pi/sim_e2e.sh
#                                    with the project root as the current directory)
#   PORT_BASE=7777 bash pi/sim_e2e.sh   first port to try (daemon = base, Teensy bus = base+1,
#                                    Teensy host link = base+2); busy ports -> base+100, +200, ...
#   QUICK=1                          shorter selftest throughput runs
#
# Steps: build (zynq sim, teensy sim, pi tools for x86 + aarch64 with -Werror) -> start fpgagpud_sim
# and teensy_sim -> gpu_selftest (x86, all tests) -> gpu_selftest (aarch64 under qemu-user, if
# available) -> gpu_stat -> gpu_demo --frames 120 (Teensy geometry) + gpu_snap PNG/PPM -> gpu_demo
# --teensy none (NET_TRIS fallback) + gpu_snap -> gpu_demo --auto with gpu_view and gpu_snap
# --frame-get alongside -> gpu_demo --view (if $DISPLAY) -> gpu_image -> PNG decode check (python
# zlib: the PNG must decode to exactly the PPM of the same frame). PNGs go to pi/build/preview/.
# Also: the Zynq address (FPGAGPU_HOST; the 10.77.0.2 -> 10.20.0.2 default against a daemon on
# every address, in a network namespace when run as root) and pi_setup.sh --dry-run next to the
# car-lan NetworkManager profile (stub nmcli).
# Exit status 0 only if every step passed.
set -u

if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then
    ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
elif [ -f pi/sim_e2e.sh ]; then
    ROOT=$(pwd)
elif [ -f sim_e2e.sh ] && [ -f libfpgagpu.c ]; then
    ROOT=$(cd .. && pwd)
else
    echo "sim_e2e.sh: run it from the project root (or pass it by path)" >&2
    exit 2
fi
PI=$ROOT/pi
BIN=$PI/build/x86
A64=$PI/build/aarch64
PREVIEW=$PI/build/preview
TMP=$(mktemp -d /tmp/pi_e2e.XXXXXX)
DAEMON=$ROOT/zynq/build/fpgagpud_sim
TSIM=$ROOT/teensy/sim/build/teensy_sim
FAILS=0
PIDS=()
NS=""

say()  { printf '\n== %s\n' "$*"; }
ok()   { printf 'OK    %s\n' "$*"; }
bad()  { printf 'FAIL  %s\n' "$*"; FAILS=$((FAILS + 1)); }

cleanup() {
    local p
    for p in "${PIDS[@]:-}"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null
    done
    wait 2>/dev/null
    [ -n "$NS" ] && ip netns del "$NS" 2>/dev/null
}
trap cleanup EXIT

# ---- 1. build ---------------------------------------------------------------------------------
say "build"
if make -C "$ROOT/zynq" sim > "$TMP/build_zynq.log" 2>&1; then ok "zynq: make sim"; else bad "zynq: make sim (see $TMP/build_zynq.log)"; fi
if make -C "$ROOT/teensy/sim" build/teensy_sim > "$TMP/build_teensy.log" 2>&1; then ok "teensy: make build/teensy_sim"; else bad "teensy sim build (see $TMP/build_teensy.log)"; fi
if make -B -C "$PI" -j8 WERROR=-Werror x86 pi > "$TMP/build_pi.log" 2>&1; then
    n=$(grep -ci 'warning' "$TMP/build_pi.log")
    if [ "$n" = 0 ]; then ok "pi: make x86 pi (-Werror, 0 warnings)"; else bad "pi: $n warnings"; fi
else
    bad "pi: build failed"; tail -30 "$TMP/build_pi.log"; exit 1
fi
[ -x "$DAEMON" ] && [ -x "$TSIM" ] || { bad "simulator binaries missing"; exit 1; }

# ---- 2. ports + simulators ---------------------------------------------------------------------
port_free() { python3 -c "import socket,sys; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1', int(sys.argv[1]))); s.close()" "$1" 2>/dev/null; }
wait_port() { local i; for i in $(seq 1 100); do python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), 0.2).close()" "$1" 2>/dev/null && return 0; sleep 0.05; done; return 1; }
BASE=${PORT_BASE:-7777}
for k in 0 1 2 3 4 5 6 7 8 9; do
    b=$((BASE + 100 * k))
    if port_free $b && port_free $((b + 1)) && port_free $((b + 2)); then BASE=$b; break; fi
done
P_NET=$BASE; P_BUS=$((BASE + 1)); P_TEENSY=$((BASE + 2))
FPGA=127.0.0.1:$P_NET
TEENSY=tcp:127.0.0.1:$P_TEENSY
say "simulators: daemon $FPGA, Teensy bus $P_BUS, Teensy host link $TEENSY (logs in $TMP)"
"$DAEMON" --sim --bind 127.0.0.1 --port "$P_NET" --bus-port "$P_BUS" > "$TMP/daemon.log" 2>&1 &
PIDS+=($!)
wait_port "$P_NET" || { bad "fpgagpud_sim did not start"; cat "$TMP/daemon.log"; exit 1; }
"$TSIM" --port "$P_TEENSY" --bus-port "$P_BUS" > "$TMP/teensy.log" 2>&1 &
PIDS+=($!)
wait_port "$P_TEENSY" || { bad "teensy_sim did not start"; cat "$TMP/teensy.log"; exit 1; }
ok "fpgagpud_sim + teensy_sim running"

# ---- 3. gpu_selftest ----------------------------------------------------------------------------
say "gpu_selftest (x86-64)"
mkdir -p "$TMP/dump"
Q=""; [ "${QUICK:-0}" = 1 ] && Q="--quick"
if timeout 300 "$BIN/gpu_selftest" --fpga "$FPGA" --teensy "$TEENSY" --dump "$TMP/dump" $Q | tee "$TMP/selftest.log"; then
    ok "gpu_selftest x86: $(tail -1 "$TMP/selftest.log")"
else
    bad "gpu_selftest x86 (dumps in $TMP/dump)"
fi

say "gpu_selftest (AArch64 Orange Pi binary under qemu-user, --quick)"
if [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || command -v qemu-aarch64-static > /dev/null; then
    runner=""
    [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || runner=qemu-aarch64-static
    if timeout 600 $runner "$A64/gpu_selftest" --fpga "$FPGA" --teensy "$TEENSY" --dump "$TMP/dump" --quick > "$TMP/selftest_a64.log" 2>&1; then
        ok "gpu_selftest aarch64: $(tail -1 "$TMP/selftest_a64.log")"
        grep -E '^(PASS|FAIL)  (tput|teensy_geom|auto_static)' "$TMP/selftest_a64.log" | sed 's/^/      /'
    else
        bad "gpu_selftest aarch64 (log $TMP/selftest_a64.log)"; grep -E '^FAIL' "$TMP/selftest_a64.log"
    fi
else
    echo "SKIP  no qemu-aarch64: AArch64 binaries not executed"
fi

say "Teensy over a serial tty: pty bridged to teensy_sim, gpu_selftest --quick --teensy /dev/pts/N"
# exercises teensy_open's tty path (termios raw, TIOCEXCL, VMIN=1 non-blocking reads, writes) --
# a pty is not a USB CDC ACM port, but the library code path is the same
python3 - "$P_TEENSY" > "$TMP/pty.name" 2> "$TMP/pty.log" <<'PYEOF' &
import os, pty, select, socket, sys, tty
m, s = pty.openpty()
tty.setraw(s)                      # keep the slave open: no hang-up between clients
print(os.ttyname(s), flush=True)
sock = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
while True:
    r, _, _ = select.select([m, sock], [], [])
    if m in r:
        d = os.read(m, 65536)
        if not d: break
        sock.sendall(d)
    if sock in r:
        d = sock.recv(65536)
        if not d: break
        os.write(m, d)
PYEOF
PTY_PID=$!
PIDS+=($PTY_PID)
for i in $(seq 1 50); do [ -s "$TMP/pty.name" ] && break; sleep 0.05; done
PTY=$(head -1 "$TMP/pty.name")
if [ -n "$PTY" ] && timeout 300 "$BIN/gpu_selftest" --fpga "$FPGA" --teensy "$PTY" --dump "$TMP/dump" --quick > "$TMP/selftest_pty.log" 2>&1; then
    ok "gpu_selftest over $PTY: $(tail -1 "$TMP/selftest_pty.log")"
    grep -E '^PASS  (teensy_hello|teensy_geom|geom_return )' "$TMP/selftest_pty.log" | sed 's/^/      /'
else
    bad "gpu_selftest over a pty ($PTY)"; grep -E '^FAIL' "$TMP/selftest_pty.log"; cat "$TMP/pty.log"
fi
kill $PTY_PID 2> /dev/null

# ---- 4. gpu_stat --------------------------------------------------------------------------------
say "gpu_stat"
if "$BIN/gpu_stat" --fpga "$FPGA" --teensy "$TEENSY" > "$TMP/stat.log" 2>&1 && grep -q "'GPU1' ok" "$TMP/stat.log"; then
    ok "gpu_stat"; sed -n '1p;/CONTROL/p;/FRAME_COUNT/p;/^teensy/p' "$TMP/stat.log" | sed 's/^/      /'
else
    bad "gpu_stat"; cat "$TMP/stat.log"
fi
if "$BIN/gpu_stat" --fpga "$FPGA" --watch 200 --count 3 > "$TMP/stat_watch.log" 2>&1 && grep -q "rates over" "$TMP/stat_watch.log"; then
    ok "gpu_stat --watch 200 --count 3"
else
    bad "gpu_stat --watch"
fi

# ---- 4b. the Zynq's address: FPGAGPU_HOST, and the default 10.77.0.2 -> 10.20.0.2 ------------------
say "Zynq address: FPGAGPU_HOST, --fpga over it, default 10.77.0.2 then 10.20.0.2"
if FPGAGPU_HOST=127.0.0.1:$P_NET "$BIN/gpu_stat" > "$TMP/stat_env.log" 2>&1 &&
   grep -q "^daemon 127.0.0.1:$P_NET " "$TMP/stat_env.log"; then
    ok "FPGAGPU_HOST=127.0.0.1:$P_NET gpu_stat (no --fpga)"
else
    bad "FPGAGPU_HOST=HOST:PORT"; cat "$TMP/stat_env.log"
fi
if FPGAGPU_HOST=127.0.0.1 "$BIN/gpu_stat" --port "$P_NET" > "$TMP/stat_env2.log" 2>&1 &&
   grep -q "^daemon 127.0.0.1:$P_NET " "$TMP/stat_env2.log"; then
    ok "FPGAGPU_HOST=127.0.0.1 gpu_stat --port $P_NET"
else
    bad "FPGAGPU_HOST=HOST + --port"; cat "$TMP/stat_env2.log"
fi
if FPGAGPU_HOST=192.0.2.1:1 "$BIN/gpu_stat" --fpga "$FPGA" > "$TMP/stat_env3.log" 2>&1 &&
   grep -q "^daemon $FPGA " "$TMP/stat_env3.log"; then
    ok "--fpga $FPGA wins over FPGAGPU_HOST"
else
    bad "--fpga over FPGAGPU_HOST"; cat "$TMP/stat_env3.log"
fi
if ! FPGAGPU_HOST=a:b:1 "$BIN/gpu_stat" > "$TMP/stat_env4.log" 2>&1 && grep -q "bad FPGAGPU_HOST" "$TMP/stat_env4.log"; then
    ok "malformed FPGAGPU_HOST refused: $(head -1 "$TMP/stat_env4.log")"
else
    bad "malformed FPGAGPU_HOST"; cat "$TMP/stat_env4.log"
fi
# The default needs the real addresses: a network namespace (root only) with the car-LAN address
# 10.20.0.2 on its loopback and a daemon on its default bind (0.0.0.0) and port (7777).
NSNAME=fgpu_e2e_$$
if [ "$(id -u)" = 0 ] && ip netns add "$NSNAME" 2> /dev/null; then
    NS=$NSNAME
    nsx() { ip netns exec "$NS" "$@"; }
    ns_wait() { local i; for i in $(seq 1 100); do nsx python3 -c "import socket; socket.create_connection(('10.20.0.2', 7777), 0.2).close()" 2>/dev/null && return 0; sleep 0.05; done; return 1; }
    nsx ip link set lo up && nsx ip addr add 10.20.0.2/32 dev lo
    nsx "$DAEMON" --sim --bus-port 0 > "$TMP/daemon_ns.log" 2>&1 &
    PIDS+=($!)
    if ns_wait; then
        # a) no route to 10.77.0.2 (the car LAN alone)
        if nsx "$BIN/gpu_stat" > "$TMP/stat_ns1.log" 2>&1 && grep -q "^daemon 10.20.0.2:7777 " "$TMP/stat_ns1.log"; then
            ok "default, 10.77.0.2 unreachable: gpu_stat found the daemon at 10.20.0.2:7777"
        else
            bad "default address, no route to 10.77.0.2"; cat "$TMP/stat_ns1.log"
        fi
        # b) 10.77.0.2 on a link that never answers: 10.20.0.2 after the short grace, not the 3 s timeout
        nsx ip link add fgd0 type dummy && nsx ip addr add 10.77.0.1/24 dev fgd0 && nsx ip link set fgd0 up
        t0=$(date +%s%N)
        if nsx "$BIN/gpu_stat" > "$TMP/stat_ns2.log" 2>&1 && grep -q "^daemon 10.20.0.2:7777 " "$TMP/stat_ns2.log"; then
            ms=$(( ($(date +%s%N) - t0) / 1000000 ))
            if [ "$ms" -lt 2000 ]; then ok "default, 10.77.0.2 silent: 10.20.0.2 used after ${ms} ms"
            else bad "default, 10.77.0.2 silent: took ${ms} ms"; fi
        else
            bad "default address, 10.77.0.2 silent"; cat "$TMP/stat_ns2.log"
        fi
        # c) both answer (the same daemon, listening on every address): 10.77.0.2 is preferred
        nsx ip addr add 10.77.0.2/32 dev lo
        if nsx "$BIN/gpu_stat" > "$TMP/stat_ns3.log" 2>&1 && grep -q "^daemon 10.77.0.2:7777 " "$TMP/stat_ns3.log"; then
            ok "default, both answer: 10.77.0.2 preferred (daemon listens on both)"
        else
            bad "default address, both reachable"; cat "$TMP/stat_ns3.log"
        fi
    else
        bad "fpgagpud_sim in the network namespace did not start"; cat "$TMP/daemon_ns.log"
    fi
else
    echo "SKIP  default-address test (needs root for a network namespace)"
fi

# ---- 4c. pi_setup.sh next to the car-lan profile (stub nmcli, --dry-run) ----------------------------
say "pi_setup.sh --dry-run: leaves the car-lan profile alone"
mkdir -p "$TMP/stub"
cat > "$TMP/stub/nmcli" <<'STUBEOF'
#!/bin/sh
echo "nmcli $*" >> "$STUB_LOG"
case "$*" in
    "-t -f RUNNING general") echo running ;;
    "-t -f NAME connection show") for p in $STUB_PROFILES; do echo "$p"; done ;;
esac
exit 0
STUBEOF
chmod +x "$TMP/stub/nmcli"
for profiles in "car-lan" "car-lan fpgagpu" "Wired"; do
    tag=$(echo "$profiles" | tr ' ' '_')
    : > "$TMP/stub_$tag.nm"
    STUB_LOG="$TMP/stub_$tag.nm" STUB_PROFILES="$profiles" PATH="$TMP/stub:$PATH" \
        bash "$PI/pi_setup.sh" --dry-run --no-install --iface lo > "$TMP/stub_$tag.log" 2>&1
    changes=$(( $(grep -cE 'connection (add|modify|up|delete)' "$TMP/stub_$tag.nm") + \
                $(grep -c '\[dry-run\] nmcli' "$TMP/stub_$tag.log") ))
    case "$profiles" in
    car-lan*)
        if [ "$changes" = 0 ] && grep -q "car LAN owns the wired port" "$TMP/stub_$tag.log"; then
            ok "profiles '$profiles': no NetworkManager change, car-lan left alone"
        else
            bad "pi_setup.sh with profiles '$profiles' ($changes changes)"; cat "$TMP/stub_$tag.log"
        fi ;;
    *)
        if grep -q "\[dry-run\] nmcli connection add type ethernet con-name fpgagpu" "$TMP/stub_$tag.log"; then
            ok "no car-lan profile: the fpgagpu profile (10.77.0.1/24) is still created"
        else
            bad "pi_setup.sh without car-lan"; cat "$TMP/stub_$tag.log"
        fi ;;
    esac
done

mkdir -p "$PREVIEW"

# ---- 5. gpu_demo (Teensy geometry) + snapshots ----------------------------------------------------
say "gpu_demo --frames 120 (Teensy transforms the meshes, Pi draws the HUD)"
if timeout 120 "$BIN/gpu_demo" --fpga "$FPGA" --teensy "$TEENSY" --frames 120 > "$TMP/demo.log" 2>&1; then
    ok "gpu_demo: $(grep 'frames shown' "$TMP/demo.log")"; grep 'Teensy' "$TMP/demo.log" | tail -2 | sed 's/^/      /'
else
    bad "gpu_demo"; cat "$TMP/demo.log"
fi
if "$BIN/gpu_snap" --fpga "$FPGA" -o "$PREVIEW/demo_teensy.png" > "$TMP/snap1.log" 2>&1 &&
   "$BIN/gpu_snap" --fpga "$FPGA" -o "$TMP/demo_teensy.ppm" >> "$TMP/snap1.log" 2>&1; then
    ok "gpu_snap READBACK -> pi/build/preview/demo_teensy.png ($(stat -c %s "$PREVIEW/demo_teensy.png") bytes)"
else
    bad "gpu_snap"; cat "$TMP/snap1.log"
fi

say "gpu_demo --teensy none --frames 60 (Pi transforms, NET_TRIS, Zynq setup)"
if timeout 120 "$BIN/gpu_demo" --fpga "$FPGA" --teensy none --frames 60 > "$TMP/demo_none.log" 2>&1; then
    ok "gpu_demo --teensy none: $(grep 'frames shown' "$TMP/demo_none.log")"
    "$BIN/gpu_snap" --fpga "$FPGA" -o "$PREVIEW/demo_pi_nettris.png" > /dev/null 2>&1 || bad "gpu_snap (fallback)"
else
    bad "gpu_demo --teensy none"; cat "$TMP/demo_none.log"
fi

# ---- 6. autonomous mode + gpu_view + FRAME_GET snapshot -------------------------------------------
say "gpu_demo --auto (Teensy renders on its own) + gpu_view + gpu_snap --frame-get alongside"
timeout 60 "$BIN/gpu_demo" --fpga "$FPGA" --teensy "$TEENSY" --auto --seconds 8 > "$TMP/demo_auto.log" 2>&1 &
AUTO_PID=$!
sleep 1.5
if "$BIN/gpu_snap" --fpga "$FPGA" --frame-get -o "$PREVIEW/demo_auto.png" > "$TMP/snap_auto.log" 2>&1 &&
   "$BIN/gpu_snap" --fpga "$FPGA" --frame-get --scale 2 -o "$TMP/auto_half.ppm" >> "$TMP/snap_auto.log" 2>&1; then
    ok "gpu_snap --frame-get (scale 1 -> pi/build/preview/demo_auto.png, scale 2): $(head -1 "$TMP/snap_auto.log")"
else
    bad "gpu_snap --frame-get"; cat "$TMP/snap_auto.log"
fi
if [ -n "${DISPLAY:-}" ]; then
    if timeout 30 "$BIN/gpu_view" --fpga "$FPGA" --seconds 3 > "$TMP/view.log" 2>&1; then
        ok "gpu_view (DISPLAY=$DISPLAY): $(tail -1 "$TMP/view.log")"
    else
        bad "gpu_view"; cat "$TMP/view.log"
    fi
    if timeout 30 "$BIN/gpu_view" --fpga "$FPGA" --scale 2 --seconds 1.5 > "$TMP/view2.log" 2>&1; then
        ok "gpu_view --scale 2: $(tail -1 "$TMP/view2.log")"
    else
        bad "gpu_view --scale 2"; cat "$TMP/view2.log"
    fi
else
    echo "SKIP  gpu_view: no DISPLAY"
fi
if wait $AUTO_PID; then
    ok "gpu_demo --auto: $(grep 'frames shown' "$TMP/demo_auto.log")"; grep 'Teensy' "$TMP/demo_auto.log" | tail -1 | sed 's/^/      /'
else
    bad "gpu_demo --auto"; cat "$TMP/demo_auto.log"
fi

# ---- 7. gpu_demo --view (in-process viewer of the returned frames) -----------------------------------
if [ -n "${DISPLAY:-}" ]; then
    say "gpu_demo --view --scale 2 --frames 120"
    if timeout 120 "$BIN/gpu_demo" --fpga "$FPGA" --teensy "$TEENSY" --view --scale 2 --frames 120 > "$TMP/demo_view.log" 2>&1; then
        ok "gpu_demo --view: $(grep 'frames shown' "$TMP/demo_view.log")"
    else
        bad "gpu_demo --view"; cat "$TMP/demo_view.log"
    fi
fi

# ---- 8. gpu_image -----------------------------------------------------------------------------------
say "gpu_image (generated 800x600 PPM, fit)"
python3 - "$TMP/test.ppm" <<'PYEOF'
import sys, math
w, h = 800, 600
px = bytearray()
for y in range(h):
    for x in range(w):
        r = int(255 * x / (w - 1)); g = int(255 * y / (h - 1))
        b = 255 if (x // 50 + y // 50) % 2 else int(128 + 127 * math.sin(x / 40.0))
        px += bytes((r, g, b))
open(sys.argv[1], 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + px)
PYEOF
if "$BIN/gpu_image" --fpga "$FPGA" "$TMP/test.ppm" > "$TMP/image.log" 2>&1 &&
   "$BIN/gpu_snap" --fpga "$FPGA" -o "$TMP/image_snap.ppm" > /dev/null 2>&1; then
    ok "gpu_image: $(cat "$TMP/image.log")"
    "$BIN/gpu_snap" --fpga "$FPGA" -o "$PREVIEW/gpu_image.png" > /dev/null 2>&1
else
    bad "gpu_image"; cat "$TMP/image.log"
fi

# ---- 9. PNG check: decode our PNGs with python zlib; must equal the PPM of the same frame ----------
say "PNG encoder check"
if python3 - "$PREVIEW/demo_teensy.png" "$TMP/demo_teensy.ppm" "$PREVIEW/demo_auto.png" "$PREVIEW/demo_pi_nettris.png" "$PREVIEW/gpu_image.png" <<'PYEOF'
import struct, sys, zlib
def png_decode(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'signature'
    pos, idat, w = 8, b'', None
    while pos < len(d):
        n, = struct.unpack('>I', d[pos:pos + 4]); t = d[pos + 4:pos + 8]; body = d[pos + 8:pos + 8 + n]
        crc, = struct.unpack('>I', d[pos + 8 + n:pos + 12 + n])
        assert zlib.crc32(t + body) & 0xffffffff == crc, 'CRC of ' + t.decode()
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10]); assert (depth, ctype) == (8, 2)
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat); stride = w * 3; out = bytearray(); prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]; line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 3] if x >= 3 else 0; b = prev[x]; c = prev[x - 3] if x >= 3 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            else: assert f == 0, 'filter %d' % f
        out += line; prev = line
    return w, h, bytes(out)
w, h, px = png_decode(sys.argv[1])
ppm = open(sys.argv[2], 'rb').read(); hdr = b'P6\n%d %d\n255\n' % (w, h)
assert ppm.startswith(hdr) and ppm[len(hdr):] == px, 'PNG pixels != PPM pixels'
colours = len(set(px[i:i + 3] for i in range(0, len(px), 3)))
print('      %s: %dx%d, decodes bit-identical to the PPM of the same frame, %d colours, %d bytes (raw %d)' %
      (sys.argv[1].split('/')[-1], w, h, colours, len(open(sys.argv[1], 'rb').read()), len(px)))
for p in sys.argv[3:]:
    w2, h2, px2 = png_decode(p)
    print('      %s: %dx%d decodes (CRCs, zlib, filters ok), %d colours' % (p.split('/')[-1], w2, h2,
          len(set(px2[i:i + 3] for i in range(0, len(px2), 3)))))
PYEOF
then ok "PNG files decode (python zlib) exactly to the PPM pixels"; else bad "PNG decode check"; fi

# ---- 10. daemon / Teensy simulator health -------------------------------------------------------------
say "simulator logs"
# gpu_selftest's "protocol" test makes the daemon reject 3 commands on purpose (observer SET_CONFIG,
# observer RECT, SPRITE_UPLOAD id 300); anything else rejected or malformed is a failure
EXPECTED='(SET_CONFIG|RECT) rejected: this client is observer|SPRITE_UPLOAD: id 300 '
n_exp=$(grep -cE "$EXPECTED" "$TMP/daemon.log")
n_sel=$(grep -c '^gpu_selftest: .* passed' "$TMP"/selftest*.log | awk -F: '{s += $2} END {print s}')
if grep -iE 'rejected|malformed|bad magic|protocol error|invalid|dropped' "$TMP/daemon.log" | grep -vqE "$EXPECTED"; then
    bad "daemon log reports unexpected rejected/malformed commands:"
    grep -iE 'rejected|malformed|bad magic|protocol error|invalid|dropped' "$TMP/daemon.log" | grep -vE "$EXPECTED" | head -5
elif [ "$n_exp" != $((3 * n_sel)) ]; then
    bad "daemon log: $n_exp deliberate rejections, expected $((3 * n_sel)) (3 per gpu_selftest run)"
else
    ok "daemon log: only the $n_exp deliberate rejections of gpu_selftest's protocol test ($(grep -c 'connected from' "$TMP/daemon.log") client connections)"
fi

say "result"
echo "preview images: $(ls "$PREVIEW" | tr '\n' ' ')"
echo "logs: $TMP"
if [ "$FAILS" = 0 ]; then echo "sim_e2e: ALL PASSED"; exit 0; fi
echo "sim_e2e: $FAILS step(s) FAILED"; exit 1
