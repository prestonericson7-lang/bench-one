#!/usr/bin/env python3
"""
test_sim.py -- end-to-end tests of fpgagpud (python3 standard library only).

Starts `fpgagpud --sim` (or the ARM build under qemu-arm with --slow) on free ports and talks
to it exactly like the Pi does (NET_* protocol, common/gpu_proto.h) and like the Teensy does
(raw 96-byte records on the simulated bus port). Every rendered frame is compared pixel for pixel
with an independent reference rendered by build/test_ref (gpu_setup + collector rules +
gpu_refrast, none of the daemon's code). Also exercises the hardware backend's probe/gating
logic against fake /dev/mem files (--fake-devmem).

    python3 test_sim.py --daemon build/fpgagpud_sim --ref build/test_ref
    python3 test_sim.py --daemon build/fpgagpud     --ref build/test_ref --slow   # ARM via qemu
Exit status 0 = all checks passed.
"""
import argparse
import array
import math
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

MAGIC = 0x31504746
REPLY = 0x8000
(HELLO, SET_CONFIG, TRIS, RECT, SPRITE_UPLOAD, SPRITE_DRAW, RECORDS, END_FRAME, WAIT_FRAME,
 STATUS, READBACK, RESET, SYNC, FRAME_GET) = range(1, 15)
OK, E_PROTO, E_NOPL, E_ARG, E_NOMEM, E_TIMEOUT = 0, -1, -2, -3, -4, -5
W, H = 1280, 720
FB_BYTES = W * H * 2
DDR_BASE = 0x1E000000
POOL_ADDR, POOL_SIZE = 0x1E400000, 0x01A00000
GPU_ID, GPU_VERSION = 0x47505531, 0x00010000
PLATFORM_ID = 0x5A702001            # the platform's pl_regs at 0x40000000 (beside the GPU at 0x43C00000)
CTL_T, CTL_P, CTL_S = 2, 4, 8
F_ZTEST, F_ZWRITE, F_NOEDGE, F_COLORKEY = 1 << 24, 1 << 25, 1 << 27, 1 << 24
R = dict(ID=0, VERSION=1, CONTROL=2, STATUS=3, FRAME_COUNT=4, FB0=5, FB1=6, FRONT=7, CLEAR=8,
         PS_FREE=9, T_LEVEL=10, OVERFLOW=11, BAD=12, T_WORDS=13, RENDER_CYCLES=14, PRIM=15,
         VSYNC=16, AXI_ERR=17, DROPPED=18, RET_ADDR=19, RET_CTRL=20, RET_STATUS=21, RET_FRAME=22,
         LAST_FRAME_NO=23)
ST_WAIT_TEENSY, ST_WAIT_PS = 1 << 4, 1 << 5

ARGS = None
TMP = None
RESULTS = {"pass": 0, "fail": 0}
LOGS = []


def check(cond, name, detail=""):
    if cond:
        RESULTS["pass"] += 1
        print(f"  PASS  {name}")
    else:
        RESULTS["fail"] += 1
        print(f"  FAIL  {name}  {detail}")
    return cond


def T(ms):
    """timeouts scale with --slow (qemu-arm)"""
    return int(ms * (8 if ARGS.slow else 1))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def f32bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def spx(seed, i):
    """sprite pixel generator, identical to test_ref.c"""
    x = (seed * 0x9E3779B1 + i * 0x85EBCA77) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x2C1B3C6D) & 0xFFFFFFFF
    x ^= x >> 12
    if ((x >> 16) & 7) == 0:
        return 0xF81F
    return x & 0xFFFF


def sprite_pixels(seed, w, h):
    return array.array("H", (spx(seed, i) for i in range(w * h))).tobytes()


def rgb565_to8(c):
    r5, g6, b5 = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)


def rect_record(x0, y0, x1, y1, c, flags=0):
    """NOEDGE rect record per SPEC 4 (z = 0)"""
    xmin, ymin, xmax, ymax = max(x0, 0), max(y0, 0), min(x1, W) - 1, min(y1, H) - 1
    r8, g8, b8 = rgb565_to8(c)
    w = [0] * 24
    w[0] = (1 << 28) | F_NOEDGE | flags | (xmax << 11) | xmin
    w[1] = (ymax << 11) | ymin
    w[14], w[17], w[20] = r8 << 16, g8 << 16, b8 << 16
    return w


def end_record(frame_no):
    w = [0] * 24
    w[0] = 15 << 28
    w[1] = frame_no & 0xFFFFFFFF
    return w


def rec_bytes(recs):
    return b"".join(struct.pack("<24I", *r) for r in recs)


# ------------------------------------------------------------------------------------------------
class Conn:
    def __init__(self, port, timeout=None):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout or T(10000) / 1000)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def send(self, t, payload=b""):
        self.s.sendall(struct.pack("<IHHI", MAGIC, t, 0, len(payload)) + payload)

    def recv_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.s.recv(min(n - len(buf), 1 << 20))
            if not chunk:
                raise ConnectionError("connection closed by the daemon")
            buf += chunk
        return bytes(buf)

    def reply(self, t):
        magic, rt, _flags, ln = struct.unpack("<IHHI", self.recv_exact(12))
        if magic != MAGIC or rt != (t | REPLY):
            raise AssertionError(f"bad reply header magic={magic:#x} type={rt:#x} (want {t | REPLY:#x})")
        return self.recv_exact(ln)

    def req(self, t, payload=b""):
        self.send(t, payload)
        return self.reply(t)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def hello(self, observer=False):
        return struct.unpack("<iIIIIIII", self.req(HELLO, struct.pack("<I", 1) if observer else b""))

    def status(self):
        v = struct.unpack("<i24IIII", self.req(STATUS))
        return v[0], list(v[1:25]), v[25], v[26], v[27]

    def regs(self):
        return self.status()[1]

    def sync(self):
        return struct.unpack("<iI", self.req(SYNC))

    def set_config(self, ctl, clear):
        return struct.unpack("<i", self.req(SET_CONFIG, struct.pack("<II", ctl, clear)))[0]

    def wait_frame(self, target, timeout_ms):
        return struct.unpack("<iI", self.req(WAIT_FRAME, struct.pack("<II", target & 0xFFFFFFFF, timeout_ms)))

    def readback(self):
        p = self.req(READBACK)
        st, w, h = struct.unpack("<iII", p[:12])
        return st, w, h, p[12:]

    def upload(self, sid, w, h, pix):
        return struct.unpack("<iII", self.req(SPRITE_UPLOAD, struct.pack("<HHHH", sid, w, h, 0) + pix))

    def reset(self):
        return struct.unpack("<i", self.req(RESET))[0]

    def frame_get(self, minf, scale, timeout_ms):
        p = self.req(FRAME_GET, struct.pack("<III", minf & 0xFFFFFFFF, scale, timeout_ms))
        st, fno, w, h = struct.unpack("<iIII", p[:16])
        return st, fno, w, h, p[16:]


class Scene:
    """One frame built twice: NET_* messages for the daemon and a test_ref script."""

    def __init__(self):
        self.msgs = []
        self.script = []
        self._tris = []

    def _flush(self):
        if self._tris:
            self.msgs.append((TRIS, struct.pack("<I", len(self._tris)) + b"".join(self._tris)))
            self._tris = []

    def tri(self, v, flags=0, cull=0):
        pay, parts = b"", []
        for (x, y, z, r, g, b) in v:
            pay += struct.pack("<fffBBBB", x, y, z, r, g, b, 0)
            parts.append(f"{f32bits(x):#x} {f32bits(y):#x} {f32bits(z):#x} {r} {g} {b}")
        self._tris.append(pay + struct.pack("<II", flags, cull))
        if cull <= 2:
            self.script.append("tri " + " ".join(parts) + f" {flags:#x} {cull}")

    def rect(self, x0, y0, x1, y1, c, z=0.0, flags=0):
        self._flush()
        self.msgs.append((RECT, struct.pack("<iiiiIfI", x0, y0, x1, y1, c, z, flags)))
        if c <= 0xFFFF:
            self.script.append(f"rect {x0} {y0} {x1} {y1} {c:#x} {f32bits(z):#x} {flags:#x}")

    def sprite(self, sid, x, y, flags=0, key=0, drawn=True):
        self._flush()
        self.msgs.append((SPRITE_DRAW, struct.pack("<HHiiI", sid, flags, x, y, key)))
        if drawn:
            self.script.append(f"sprite {sid} {x} {y} {flags} {key:#x}")

    def records(self, recs, in_script=True):
        self._flush()
        self.msgs.append((RECORDS, struct.pack("<I", len(recs)) + rec_bytes(recs)))
        if in_script:
            self.script += ["rec " + " ".join(f"{w:#x}" for w in r) for r in recs]

    def end(self, frame_no):
        self._flush()
        self.msgs.append((END_FRAME, struct.pack("<I", frame_no & 0xFFFFFFFF)))

    def send(self, conn):
        self._flush()
        for t, p in self.msgs:
            conn.send(t, p)


def reference(lines, clear):
    path = os.path.join(TMP, "ref.txt")
    out = os.path.join(TMP, "ref.bin")
    with open(path, "w") as f:
        f.write(f"clear {clear:#x}\n")
        f.write("\n".join(lines) + "\n")
        f.write(f"render {out}\n")
    subprocess.run([ARGS.ref, path], check=True, stdout=subprocess.DEVNULL)
    with open(out, "rb") as f:
        return f.read()


def compare(got, want, name):
    if got == want:
        return check(True, name)
    if len(got) != len(want):
        return check(False, name, f"size {len(got)} != {len(want)}")
    a, b = array.array("H", got), array.array("H", want)
    diffs = [i for i in range(len(a)) if a[i] != b[i]]
    ex = ", ".join(f"({i % W},{i // W}) got {a[i]:#06x} want {b[i]:#06x}" for i in diffs[:4])
    return check(False, name, f"{len(diffs)} pixels differ: {ex}")


def downscale2(fb):
    a = array.array("H", fb)
    out = array.array("H", (a[(2 * y) * W + 2 * x] for y in range(H // 2) for x in range(W // 2)))
    return out.tobytes()


class Daemon:
    def __init__(self, extra, name):
        self.port = free_port()
        self.bus = free_port()
        self.name = name
        self.logpath = os.path.join(TMP, f"daemon-{name}.log")
        self.log = open(self.logpath, "w")
        cmd = [ARGS.daemon, "--port", str(self.port), "--bus-port", str(self.bus), "-v"] + extra
        self.p = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)
        LOGS.append(self)
        t0 = time.time()
        while True:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                if self.p.poll() is not None or time.time() - t0 > T(10000) / 1000:
                    raise RuntimeError(f"daemon {name} did not start; see {self.logpath}")
                time.sleep(0.05)
        # the probe connection above was accepted and closed; give the daemon a moment to log it
        time.sleep(0.05)

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
        self.log.close()

    def logtext(self):
        with open(self.logpath) as f:
            return f.read()


def bus_send(port, recs, chunk=None):
    s = socket.create_connection(("127.0.0.1", port), timeout=T(10000) / 1000)
    data = rec_bytes(recs)
    if chunk:
        for i in range(0, len(data), chunk):
            s.sendall(data[i:i + chunk])
            time.sleep(0.001)
    else:
        s.sendall(data)
    return s


def wait_regs(conn, pred, timeout_ms, what):
    t0 = time.time()
    while True:
        regs = conn.regs()
        if pred(regs):
            return regs
        if time.time() - t0 > timeout_ms / 1000:
            check(False, what, f"timed out; regs={regs}")
            return regs
        time.sleep(0.01)


def random_scene(rng, sc, ntris, nrects):
    for _ in range(ntris):
        kind = rng.random()
        if kind < 0.7:
            cx, cy, sz = rng.uniform(-100, 1380), rng.uniform(-100, 820), rng.uniform(4, 250)
        elif kind < 0.9:
            cx, cy, sz = rng.uniform(-400, 1700), rng.uniform(-400, 1100), rng.uniform(300, 2500)
        else:
            cx, cy, sz = rng.uniform(0, 1280), rng.uniform(0, 720), rng.uniform(0.2, 3)
        v = [(cx + rng.uniform(-sz, sz), cy + rng.uniform(-sz, sz), rng.uniform(-0.1, 1.1),
              rng.randrange(256), rng.randrange(256), rng.randrange(256)) for _ in range(3)]
        flags = rng.choice([0, F_ZTEST | F_ZWRITE, F_ZTEST | F_ZWRITE, F_ZTEST, F_ZWRITE])
        sc.tri(v, flags, rng.choice([0, 0, 1, 2]))
    for _ in range(nrects):
        x0, y0 = rng.randrange(-100, 1300), rng.randrange(-100, 740)
        sc.rect(x0, y0, x0 + rng.randrange(-5, 300), y0 + rng.randrange(-5, 200), rng.randrange(65536),
                rng.uniform(0, 1), rng.choice([0, F_ZTEST, F_ZTEST | F_ZWRITE]))


def draw_and_check(ctl, obs, sc, frame_no, clear, name):
    """send a scene + END_FRAME, wait for the swap, READBACK, compare with the reference"""
    fc = ctl.regs()[R["FRAME_COUNT"]]
    sc.end(frame_no)
    sc.send(ctl)
    st, fc2 = ctl.wait_frame(fc + 1, T(5000))
    check(st == OK and fc2 == fc + 1, f"{name}: WAIT_FRAME -> frame {fc + 1}", f"status {st} fc {fc2}")
    st, w, h, fb = (obs or ctl).readback()
    check(st == OK and (w, h) == (W, H), f"{name}: READBACK header", f"{st} {w}x{h}")
    ref = reference(sc.script, clear)
    compare(fb, ref, f"{name}: READBACK == reference ({len(sc.script)} primitives)")
    return fb, ref


# ------------------------------------------------------------------------------------------------
def test_basic_and_render(d):
    print("[basic protocol, rendering, sprites, pool]")
    ctl = Conn(d.port)
    st, pv, pid, pver, w, h, pool, maxspr = ctl.hello()
    check((st, pv, pid, pver, w, h, pool, maxspr) == (OK, 1, GPU_ID, GPU_VERSION, W, H, POOL_SIZE, 256),
          "HELLO reply (controller)", str((st, pv, hex(pid), hex(pver), w, h, pool, maxspr)))
    st, regs, derr, drec, pused = ctl.status()
    check(st == OK and regs[R["ID"]] == GPU_ID and regs[R["VERSION"]] == GPU_VERSION, "STATUS: ID/VERSION")
    check(regs[R["CONTROL"]] == CTL_P | CTL_S, "HELLO set CONTROL = SRC_PS|SCANOUT_EN", hex(regs[R["CONTROL"]]))
    check(regs[R["CLEAR"]] == 0x0010 and regs[R["FRAME_COUNT"]] >= 1, "start-up frame: clear 0x0010 rendered",
          f"clear {regs[R['CLEAR']]:#x} fc {regs[R['FRAME_COUNT']]}")
    check(regs[R["LAST_FRAME_NO"]] == 0 and drec >= 1, "start-up END(0) pushed", f"{regs[R['LAST_FRAME_NO']]} {drec}")
    st, w, h, fb = ctl.readback()
    check(st == OK and fb == struct.pack("<H", 0x0010) * (W * H), "READBACK of the start-up frame is all 0x0010")

    check(ctl.set_config(CTL_P | CTL_S, 0x1234) == OK, "SET_CONFIG ok")
    check(ctl.set_config(CTL_P | CTL_S | 1, 0) == E_ARG, "SET_CONFIG with SOFT_RESET bit -> GPU_ERR_ARG")
    check(ctl.set_config(CTL_P | 0x10, 0) == E_ARG, "SET_CONFIG with unknown bit -> GPU_ERR_ARG")
    check(ctl.set_config(CTL_P, 0x10000) == E_ARG, "SET_CONFIG clear > 0xFFFF -> GPU_ERR_ARG")
    check(ctl.set_config(CTL_P | CTL_S, 0x1234) == OK, "SET_CONFIG restored")
    check(ctl.sync() == (OK, 3), "SYNC reports the 3 rejected SET_CONFIGs (and resets the count)")

    # frame 1: random triangles (clipping, culling, z) + rects
    rng = random.Random(1234)
    sc = Scene()
    random_scene(rng, sc, 260 if not ARGS.slow else 120, 40)
    # a few special triangles: NaN / huge / degenerate
    sc.tri([(float("nan"), 5, 0.5, 1, 2, 3), (10, 10, 0.5, 1, 2, 3), (20, 5, 0.5, 1, 2, 3)])
    sc.tri([(-1e30, -1e30, 0.5, 9, 9, 9), (1e30, 0, 0.5, 9, 9, 9), (0, 1e30, 0.5, 9, 9, 9)])
    sc.tri([(5, 5, 0.5, 1, 1, 1), (5, 5, 0.5, 1, 1, 1), (9, 9, 0.5, 1, 1, 1)])
    draw_and_check(ctl, None, sc, 1, 0x1234, "frame 1 (random tris + rects)")
    check(ctl.sync() == (OK, 0), "SYNC after a valid frame: 0 errors")
    regs = ctl.regs()
    check(regs[R["LAST_FRAME_NO"]] == 1 and regs[R["BAD"]] == 0 and regs[R["OVERFLOW"]] == 0,
          "LAST_FRAME_NO=1, no bad records, no overflow",
          f"{regs[R['LAST_FRAME_NO']]} {regs[R['BAD']]} {regs[R['OVERFLOW']]}")

    # sprites + pool allocation
    sp = {}
    for sid, w, h, seed in ((1, 64, 48, 11), (2, 36, 20, 22), (3, 320, 200, 33)):
        st, addr, stride = ctl.upload(sid, w, h, sprite_pixels(seed, w, h))
        sp[sid] = (addr, stride, w, h, seed)
    a1, a2, a3 = sp[1][0], sp[2][0], sp[3][0]
    check(sp[1][:2] == (POOL_ADDR, 128), "sprite 1 at pool base, stride 128", str(sp[1]))
    check(sp[2][:2] == (POOL_ADDR + 128 * 48, 72), "sprite 2 bump-allocated, stride 72 (36 px)", str(sp[2]))
    check(sp[3][:2] == (a2 + 72 * 20, 640) and a3 % 8 == 0, "sprite 3 bump-allocated, 8-byte aligned", str(sp[3]))
    check(ctl.status()[4] == 128 * 48 + 72 * 20 + 640 * 200, "STATUS pool_used")
    sc = Scene()
    for sid, (addr, stride, w, h, seed) in sp.items():
        sc.script.append(f"upload {sid} {w} {h} {seed} {addr:#x} {stride}")
    sc.rect(0, 0, 1280, 720, 0x0841)
    sc.sprite(1, 100, 100)
    sc.sprite(2, -8, -5)                            # negative x/y: clipped by advancing src
    sc.sprite(1, 1260, 700)                         # PL clips at the right/bottom edge
    sc.sprite(3, 400, 300, 1, 0xF81F)               # colour key
    sc.sprite(3, -320, 10, drawn=False)             # fully off-screen -> no record
    sc.sprite(1, 1280, 10, drawn=False)             # x >= 1280 -> no record
    sc.sprite(3, 700, -150)                         # partly above the screen
    # raw SPRITE record from the pool (a 64x32 window into sprite 3, colour-keyed)
    raw = [0] * 24
    raw[0] = (2 << 28) | F_COLORKEY
    raw[1] = (600 << 11) | 900
    raw[2] = (32 << 11) | 64
    raw[3] = a3 + 10 * 640 + 16 * 2
    raw[4] = 640
    raw[5] = 0xF81F
    sc.records([raw, rect_record(1000, 650, 1100, 700, 0xFFE0)])
    draw_and_check(ctl, None, sc, 2, 0x1234, "frame 2 (sprites, clipping, colour key, raw records)")
    check(ctl.sync() == (OK, 0), "SYNC: 0 errors after the sprite frame")

    # re-upload rules
    st, addr, stride = ctl.upload(2, 16, 10, sprite_pixels(44, 16, 10))
    check((st, addr, stride) == (OK, a2, 32), "re-upload that fits reuses the slot", str((st, hex(addr), stride)))
    st, addr, stride = ctl.upload(1, 128, 64, sprite_pixels(55, 128, 64))
    check((st, addr) == (OK, a3 + 640 * 200), "re-upload that does not fit is bump-allocated", hex(addr))
    sc = Scene()
    sc.script.append(f"upload 2 16 10 44 {a2:#x} 32")
    sc.script.append(f"upload 1 128 64 55 {addr:#x} 256")
    sc.sprite(2, 4, 4)
    sc.sprite(1, 200, 8)
    draw_and_check(ctl, None, sc, 3, 0x1234, "frame 3 (re-uploaded sprites)")

    # pool exhaustion and RESET
    check(ctl.reset() == OK, "RESET")
    check(ctl.status()[4] == 0, "RESET freed the pool")
    big = sprite_pixels(7, 2048, 16) * (2047 // 16) + sprite_pixels(7, 2048, 2047 % 16)
    res = [ctl.upload(10 + i, 2048, 2047, big) for i in range(4)]
    check([r[0] for r in res] == [OK, OK, OK, E_NOMEM], "3 x 8 MB uploads fit, the 4th -> GPU_ERR_NOMEM",
          str([r[0] for r in res]))
    check([r[1] for r in res[:3]] == [POOL_ADDR + i * 4096 * 2047 for i in range(3)], "big uploads contiguous")
    check(ctl.reset() == OK and ctl.upload(5, 4, 1, b"\x01\x00" * 4)[1] == POOL_ADDR, "after RESET the pool starts over")
    ctl.close()


def test_errors(d):
    print("[malformed input, roles, robustness]")
    ctl = Conn(d.port)
    ctl.hello()
    ctl.set_config(CTL_P | CTL_S, 0)
    check(ctl.sync() == (OK, 0), "SYNC starts at 0 errors")
    # TRIS whose count does not match the payload
    ctl.send(TRIS, struct.pack("<I", 3) + b"\0" * 56 * 2)
    check(ctl.sync() == (OK, 1), "TRIS with wrong length: dropped, 1 error")
    sc = Scene()
    sc.tri([(10, 10, 0.5, 255, 0, 0), (100, 10, 0.5, 255, 0, 0), (10, 100, 0.5, 255, 0, 0)], 0, 7)  # bad cull
    sc.tri([(10, 10, 0.5, 0, 255, 0), (100, 10, 0.5, 0, 255, 0), (10, 100, 0.5, 0, 255, 0)], 0, 0)
    sc.rect(0, 0, 5, 5, 0x10000)                    # not RGB565
    sc.sprite(200, 0, 0, drawn=False)               # never uploaded
    sc.send(ctl)
    check(ctl.sync() == (OK, 3), "bad cull / bad colour / unknown sprite: 3 errors, the valid triangle kept")
    st, a, s = ctl.upload(9, 8, 2, sprite_pixels(1, 8, 2))
    sc2 = Scene()
    sc2.sprite(9, 2, 0, drawn=False)                # x not a multiple of 4
    sc2.send(ctl)
    check(ctl.sync() == (OK, 1), "SPRITE_DRAW at x=2 rejected")
    check(ctl.upload(9, 6, 2, b"\0" * 24)[0] == E_ARG, "SPRITE_UPLOAD w=6 -> GPU_ERR_ARG")
    check(ctl.upload(300, 8, 2, b"\0" * 32)[0] == E_ARG, "SPRITE_UPLOAD id 300 -> GPU_ERR_ARG")
    p = struct.pack("<HHHH", 9, 8, 2, 0) + b"\0" * 30
    check(struct.unpack("<iII", ctl.req(SPRITE_UPLOAD, p))[0] == E_PROTO, "SPRITE_UPLOAD short payload -> GPU_ERR_PROTO")
    check(ctl.frame_get(0, 3, 10)[0] == E_ARG, "FRAME_GET scale 3 -> GPU_ERR_ARG")
    check(struct.unpack("<i", ctl.req(0x55, b"abc"))[0] == E_PROTO, "unknown type -> GPU_ERR_PROTO reply")
    check(ctl.req(HELLO, b"\0" * 8)[:4] == struct.pack("<i", E_PROTO), "HELLO with 8-byte payload -> GPU_ERR_PROTO")
    check(ctl.req(STATUS, b"x")[:4] == struct.pack("<i", E_PROTO), "STATUS with a payload -> GPU_ERR_PROTO")
    n = ctl.sync()[1]
    check(n == 7, "all 7 of these were counted", str(n))
    # raw SPRITE records (NET_RECORDS) may only read the GPU's DDR window
    def spr_rec(x, y, w, h, src, stride):
        r = [0] * 24
        r[0], r[1], r[2], r[3], r[4] = 2 << 28, (y << 11) | x, (h << 11) | w, src, stride
        return r
    WIN_END = DDR_BASE + 0x2000000
    cases = [
        (spr_rec(0, 0, 64, 8, 0x00100000, 128), 1, "kernel memory (0x00100000)"),
        (spr_rec(0, 0, 64, 8, DDR_BASE - 8, 128), 1, "8 bytes below the window"),
        (spr_rec(0, 0, 64, 8, WIN_END - 7 * 128 - 128 + 8, 128), 1, "8 bytes past the window end"),
        (spr_rec(0, 0, 64, 8, WIN_END - 7 * 128 - 128, 128), 0, "last row ends exactly at the window end"),
        (spr_rec(0, 700, 4, 720, POOL_ADDR, 0x10000), 0, "rows clipped at the bottom edge stay inside"),
        (spr_rec(0, 690, 4, 720, WIN_END - 29 * 0x10000, 0x10000), 1, "30 visible rows: the last one leaves the window"),
        (spr_rec(1276, 0, 64, 1, WIN_END - 8, 8), 0, "columns clipped at the right edge (4 px read)"),
        (spr_rec(1280, 0, 64, 1, 0, 8), 0, "off-screen sprite reads nothing (accepted)"),
        (spr_rec(0, 0, 0, 5, 0, 8), 0, "w = 0 reads nothing (accepted)"),
    ]
    for rec, errs, what in cases:
        ctl.send(RECORDS, struct.pack("<I", 1) + rec_bytes([rec]))
        got = ctl.sync()
        check(got == (OK, errs), f"raw SPRITE record: {what} -> {'dropped' if errs else 'accepted'}", str(got))
    fcx = ctl.regs()[R["FRAME_COUNT"]]
    ctl.send(END_FRAME, struct.pack("<I", 49))
    ctl.wait_frame(fcx + 1, T(3000))
    prim = ctl.regs()[R["PRIM"]]
    check(prim == 1 + 5, "the valid triangle + the 5 accepted sprite records reached the list", str(prim))

    # the valid triangle from above must be in the next frame
    sc3 = Scene()
    sc3.tri([(10, 10, 0.5, 0, 255, 0), (100, 10, 0.5, 0, 255, 0), (10, 100, 0.5, 0, 255, 0)], 0, 0)
    sc3.send(ctl)
    fc = ctl.regs()[R["FRAME_COUNT"]]
    ctl.send(END_FRAME, struct.pack("<I", 50))
    ctl.wait_frame(fc + 1, T(3000))
    compare(ctl.readback()[3], reference(sc3.script, 0), "frame after errors holds exactly the valid triangle")
    check(ctl.sync() == (OK, 0), "no errors from that frame")

    # byte-by-byte delivery (short reads)
    msg = struct.pack("<IHHI", MAGIC, STATUS, 0, 0) + struct.pack("<IHHI", MAGIC, SYNC, 0, 0)
    for i in range(len(msg)):
        ctl.s.sendall(msg[i:i + 1])
        time.sleep(0.003)
    st = struct.unpack("<i", ctl.reply(STATUS)[:4])[0]
    check(st == OK and ctl.reply(SYNC)[:4] == struct.pack("<i", OK), "messages split into 1-byte segments")

    # observers and clients without HELLO
    obs = Conn(d.port)
    check(obs.hello(observer=True)[0] == OK, "observer HELLO")
    obs.send(RECT, struct.pack("<iiiiIfI", 0, 0, 10, 10, 0xFFFF, 0.0, 0))
    check(obs.set_config(CTL_P, 0) == E_PROTO, "observer SET_CONFIG rejected (GPU_ERR_PROTO)")
    check(obs.sync() == (OK, 2), "observer RECT + SET_CONFIG counted as 2 errors, connection kept")
    check(obs.status()[0] == OK and obs.readback()[0] == OK, "observer STATUS / READBACK allowed")
    nohello = Conn(d.port)
    nohello.send(END_FRAME, struct.pack("<I", 1))
    check(nohello.sync() == (OK, 1), "client without HELLO cannot draw")
    # controller take-over: obs2 becomes controller, ctl is demoted
    obs2 = Conn(d.port)
    obs2.hello()
    ctl.send(END_FRAME, struct.pack("<I", 1))
    check(ctl.sync() == (OK, 1), "old controller demoted: its END_FRAME rejected")
    check(obs2.regs()[R["CONTROL"]] == CTL_P | CTL_S, "take-over did soft reset + CONTROL=SRC_PS|SCANOUT_EN")
    # 5th connection evicts the longest-silent non-controller (nohello)
    time.sleep(0.05)
    obs.status(); ctl.status(); obs2.status()
    fifth = Conn(d.port)
    check(fifth.hello(observer=True)[0] == OK, "5th client admitted")
    try:
        nohello.s.settimeout(T(2000) / 1000)
        gone = nohello.s.recv(1) == b""
    except OSError:
        gone = True
    check(gone, "longest-silent non-controller evicted")
    for c in (obs, fifth):
        c.close()

    # connections the daemon must drop, without affecting others
    bad = Conn(d.port)
    bad.s.sendall(struct.pack("<IHHI", 0xDEADBEEF, STATUS, 0, 0))
    bad.s.settimeout(T(2000) / 1000)
    check(bad.s.recv(1) == b"", "bad magic -> connection closed")
    bad = Conn(d.port)
    bad.s.sendall(struct.pack("<IHHI", MAGIC, TRIS, 0, 8 * 1024 * 1024 + 1))
    bad.s.settimeout(T(2000) / 1000)
    check(bad.s.recv(1) == b"", "payload > 8 MB -> connection closed")
    bad = Conn(d.port)
    bad.hello(observer=True)
    bad.s.sendall(struct.pack("<IHHI", MAGIC, TRIS, 0, 5000) + b"\0" * 100)
    bad.close()                                     # disconnect mid-message
    bad = Conn(d.port)
    bad.hello(observer=True)
    bad.send(WAIT_FRAME, struct.pack("<II", 0xFFFFFFF0, 60000))
    bad.close()                                     # disconnect while parked
    time.sleep(0.2)
    check(d.alive() and obs2.status()[0] == OK, "daemon alive and serving after abusive clients")
    # messages sent right before a close are still executed
    last = Conn(d.port)
    last.hello()
    fc = last.regs()[R["FRAME_COUNT"]]
    last.send(RECT, struct.pack("<iiiiIfI", 0, 0, 1280, 720, 0xF800, 0.0, 0))
    last.send(END_FRAME, struct.pack("<I", 77))
    last.s.shutdown(socket.SHUT_WR)
    last.s.settimeout(T(2000) / 1000)
    check(last.s.recv(1) == b"", "half-closed client is closed after its messages ran")
    st, fc2 = obs2.wait_frame(fc + 1, T(3000))
    check(st == OK and obs2.regs()[R["LAST_FRAME_NO"]] == 77, "its frame was rendered")
    last.close()
    obs2.close()
    ctl.close()


def test_wait_and_frame_get(d):
    print("[WAIT_FRAME, FRAME_GET (two-way path)]")
    ctl = Conn(d.port)
    ctl.hello()
    ctl.set_config(CTL_P | CTL_S, 0x0000)
    obs = Conn(d.port)
    obs.hello(observer=True)
    fc = ctl.regs()[R["FRAME_COUNT"]]
    check(ctl.wait_frame(fc, 0) == (OK, fc), "WAIT_FRAME for the current count returns at once")
    t0 = time.time()
    st, fc2 = ctl.wait_frame(fc + 1, 150)
    dt = time.time() - t0
    check(st == E_TIMEOUT and fc2 == fc and 0.14 <= dt < T(2000) / 1000, "WAIT_FRAME timeout", f"{st} {dt:.3f}s")

    # FRAME_GET parked in an observer, satisfied by the controller's next frame
    base = 1000
    obs.send(FRAME_GET, struct.pack("<III", base, 1, T(5000)))
    obs2 = Conn(d.port)
    obs2.hello(observer=True)
    obs2.send(WAIT_FRAME, struct.pack("<II", fc + 1, T(5000)))
    time.sleep(0.05)
    t0 = time.time()
    st = ctl.status()
    dt = time.time() - t0
    check(st[0] == OK and dt < T(500) / 1000, f"controller served while 2 observers are parked ({dt * 1000:.1f} ms)")
    check(st[1][R["RET_CTRL"]] & 1 == 1, "RET_ENABLE set while a FRAME_GET is pending", hex(st[1][R["RET_CTRL"]]))
    rng = random.Random(99)
    sc = Scene()
    random_scene(rng, sc, 60, 10)
    fb, ref = draw_and_check(ctl, None, sc, base, 0x0000, "frame for FRAME_GET")
    st, fc3 = struct.unpack("<iI", obs2.reply(WAIT_FRAME))
    check(st == OK and fc3 == fc + 1, "parked WAIT_FRAME of another client completed", f"{st} {fc3}")
    p = obs.reply(FRAME_GET)
    st, fno, w, h = struct.unpack("<iIII", p[:16])
    check((st, fno, w, h) == (OK, base, W, H), "FRAME_GET reply header", str((st, fno, w, h)))
    compare(p[16:], ref, "FRAME_GET scale 1 pixels == reference")
    # served again from the cache
    st, fno, w, h, px = obs.frame_get(base, 1, 100)
    check(st == OK and fno == base and px == ref, "FRAME_GET of the same frame served from the cache")
    # scale 2
    obs.send(FRAME_GET, struct.pack("<III", base + 1, 2, T(5000)))
    sc = Scene()
    random_scene(random.Random(7), sc, 40, 5)
    fb, ref = draw_and_check(ctl, None, sc, base + 1, 0x0000, "frame for FRAME_GET scale 2")
    p = obs.reply(FRAME_GET)
    st, fno, w, h = struct.unpack("<iIII", p[:16])
    check((st, fno, w, h) == (OK, base + 1, W // 2, H // 2), "FRAME_GET scale 2 header", str((st, fno, w, h)))
    compare(p[16:], downscale2(ref), "FRAME_GET scale 2 pixels == every 2nd pixel of every 2nd row")
    # a captured frame older than min_frame_no is released and the next one is taken
    obs.send(FRAME_GET, struct.pack("<III", base + 3, 1, T(5000)))
    for k in (2, 3):
        sc = Scene()
        sc.rect(0, 0, 1280, 720, 0x1111 * k)
        draw_and_check(ctl, None, sc, base + k, 0x0000, f"frame {base + k}")
    p = obs.reply(FRAME_GET)
    st, fno = struct.unpack("<iI", p[:8])
    check(st == OK and fno == base + 3 and p[16:] == struct.pack("<H", 0x3333) * (W * H),
          "FRAME_GET skips a too-old captured frame and returns the next", f"{st} {fno}")
    st, fno, w, h, px = obs.frame_get(base + 50, 1, 200)
    check(st == E_TIMEOUT and (w, h) == (0, 0), "FRAME_GET timeout -> GPU_ERR_TIMEOUT", str(st))
    time.sleep(0.4)
    regs = ctl.regs()
    check(regs[R["RET_CTRL"]] == 0, "RET_ENABLE cleared again once nobody waits", hex(regs[R["RET_CTRL"]]))
    for c in (obs, obs2, ctl):
        c.close()


def test_teensy_barrier(d):
    print("[simulated Teensy bus: barrier ordering, drain, framing]")
    ctl = Conn(d.port)
    ctl.hello()
    obs = Conn(d.port)
    obs.hello(observer=True)
    check(ctl.set_config(CTL_T | CTL_P | CTL_S, 0x0000) == OK, "CONTROL = SRC_TEENSY|SRC_PS|SCANOUT_EN")
    regs0 = obs.regs()
    fc = regs0[R["FRAME_COUNT"]]
    check(regs0[R["STATUS"]] & ST_WAIT_TEENSY, "collector waits for the Teensy part first", hex(regs0[R["STATUS"]]))

    # PS part first in time: 40 rects (960 words > 512-entry PS FIFO) + END(500)
    ps = Scene()
    for i in range(40):
        x, y = 40 + (i % 10) * 120, 60 + (i // 10) * 150
        ps.rect(x, y, x + 100, y + 120, 0xF800 if i % 2 else 0x07E0)
    ps.end(500)
    ps.send(ctl)
    time.sleep(0.2 if not ARGS.slow else 1.0)
    regs = obs.regs()
    check(regs[R["FRAME_COUNT"]] == fc, "no frame before the Teensy END")
    check(regs[R["STATUS"]] & ST_WAIT_TEENSY and regs[R["PS_FREE"]] < 24,
          "PS FIFO full while the collector waits for the Teensy (daemon stalled, not blocked)",
          f"status {regs[R['STATUS']]:#x} free {regs[R['PS_FREE']]}")
    check(obs.sync()[0] == OK, "observer served while the controller is stalled")
    check(obs.wait_frame(fc + 1, 200)[0] == E_TIMEOUT, "WAIT_FRAME times out while the barrier holds")

    # Teensy part: full-screen blue, a yellow band over the PS rects, END(77)
    trecs = [rect_record(0, 0, 1280, 720, 0x001F), rect_record(0, 100, 1280, 400, 0xFFE0)]
    tw0 = regs[R["T_WORDS"]]
    bus = bus_send(d.bus, trecs + [end_record(77)])
    st, fc2 = obs.wait_frame(fc + 1, T(5000))
    check(st == OK, "frame completes once the Teensy END arrives", f"{st} {fc2}")
    regs = ctl.regs()
    check(regs[R["LAST_FRAME_NO"]] == 500, "frame_no comes from the PS END (SRC_PS enabled)", str(regs[R["LAST_FRAME_NO"]]))
    check(regs[R["T_WORDS"]] - tw0 == 72, "T_WORDS counted 3 records", str(regs[R["T_WORDS"]] - tw0))
    tlines = ["rec " + " ".join(f"{w:#x}" for w in r) for r in trecs]
    st, w, h, fb = ctl.readback()
    ref = reference(tlines + ps.script, 0x0000)
    compare(fb, ref, "barrier: Teensy records drawn first, PS records on top")
    wrong = reference(ps.script + tlines, 0x0000)
    check(fb != wrong, "(the opposite order would give a different picture)")

    # Teensy part first in time this time
    fc = regs[R["FRAME_COUNT"]]
    bus.sendall(rec_bytes([rect_record(0, 0, 1280, 720, 0x780F), end_record(78)]))
    time.sleep(0.1)
    check(obs.regs()[R["FRAME_COUNT"]] == fc, "Teensy END alone does not complete the list (PS enabled)")
    ps2 = Scene()
    ps2.rect(500, 300, 700, 400, 0x07FF)
    ps2.end(501)
    ps2.send(ctl)
    st, _ = obs.wait_frame(fc + 1, T(5000))
    ref = reference(["rec " + " ".join(f"{w:#x}" for w in rect_record(0, 0, 1280, 720, 0x780F))] + ps2.script, 0)
    compare(ctl.readback()[3], ref, "Teensy-first arrival gives the same ordering")

    # Teensy-only mode: frame_no from the Teensy END
    check(ctl.set_config(CTL_T | CTL_S, 0x0000) == OK, "CONTROL = SRC_TEENSY|SCANOUT_EN")
    fc = ctl.regs()[R["FRAME_COUNT"]]
    bus.sendall(rec_bytes([rect_record(10, 10, 50, 50, 0xFFFF), end_record(4242)]), )
    st, _ = obs.wait_frame(fc + 1, T(5000))
    regs = ctl.regs()
    check(st == OK and regs[R["LAST_FRAME_NO"]] == 4242, "Teensy-only frame, frame_no from the Teensy END")
    # a partial record (Teensy disconnects mid-record) is discarded by the next SOR
    bad0 = regs[R["BAD"]]
    bus.sendall(rec_bytes([rect_record(0, 0, 8, 8, 0xFFFF)])[:50])
    bus.close()
    time.sleep(0.1)
    bus = bus_send(d.bus, [rect_record(20, 20, 60, 60, 0x07E0), end_record(4243)], chunk=7)
    st, _ = obs.wait_frame(fc + 2, T(5000))
    regs = ctl.regs()
    check(st == OK and regs[R["BAD"]] == bad0 + 1, "partial record on the bus: BAD_RECORDS += 1",
          f"{regs[R['BAD']] - bad0}")
    ref = reference(["rec " + " ".join(f"{w:#x}" for w in rect_record(20, 20, 60, 60, 0x07E0))], 0)
    compare(ctl.readback()[3], ref, "record after the partial one is intact (7-byte TCP segments)")

    # Teensy disabled: its words are drained and counted
    check(ctl.set_config(CTL_P | CTL_S, 0) == OK, "CONTROL = SRC_PS|SCANOUT_EN")
    dr0 = ctl.regs()[R["DROPPED"]]
    fc = ctl.regs()[R["FRAME_COUNT"]]
    bus.sendall(rec_bytes([rect_record(0, 0, 1280, 720, 0xFFFF)] * 3))
    regs = wait_regs(obs, lambda r: r[R["DROPPED"]] - dr0 >= 72, T(3000), "drained words counted")
    check(regs[R["DROPPED"]] - dr0 == 72 and regs[R["FRAME_COUNT"]] == fc, "disabled Teensy source drained (DROPPED += 72)")
    bus.close()

    # list overflow: 1600 records in one frame (RECORDS message), 64 dropped
    ov0 = regs[R["OVERFLOW"]]
    sc = Scene()
    recs = [rect_record(i % 1270, (i * 7) % 710, i % 1270 + 10, (i * 7) % 710 + 10, (i * 37) & 0xFFFF)
            for i in range(1600)]
    sc.records(recs)
    draw_and_check(ctl, obs, sc, 9000, 0, "1600-record frame (list keeps the first 1536)")
    regs = ctl.regs()
    check(regs[R["OVERFLOW"]] - ov0 == 64 and regs[R["PRIM"]] == 1536, "LIST_OVERFLOW += 64, PRIM_COUNT 1536",
          f"{regs[R['OVERFLOW']] - ov0} {regs[R['PRIM']]}")
    ctl.close()
    obs.close()


def test_nopl():
    print("[PL not configured (--sim-nopl) and configured later (--sim-pl-delay-ms)]")
    d = Daemon(["--sim", "--sim-nopl"], "nopl")
    try:
        c = Conn(d.port)
        h = c.hello()
        check(h[0] == E_NOPL and h[1] == 1 and h[2] == 0, "HELLO -> GPU_ERR_NOPL", str(h))
        st, regs, derr, drec, pused = c.status()
        check(st == E_NOPL and regs == [0] * 24, "STATUS -> GPU_ERR_NOPL, registers not read")
        c.send(RECT, struct.pack("<iiiiIfI", 0, 0, 10, 10, 0, 0.0, 0))
        check(c.sync() == (E_NOPL, 1), "drawing dropped and counted; SYNC -> GPU_ERR_NOPL")
        check(c.readback()[:3] == (E_NOPL, 0, 0), "READBACK -> GPU_ERR_NOPL")
        check(c.wait_frame(1, 10) == (E_NOPL, 0), "WAIT_FRAME -> GPU_ERR_NOPL")
        check(c.set_config(CTL_P, 0) == E_NOPL, "SET_CONFIG -> GPU_ERR_NOPL")
        check(c.upload(1, 4, 1, b"\0" * 8)[0] == E_NOPL, "SPRITE_UPLOAD -> GPU_ERR_NOPL")
        check(c.frame_get(0, 1, 10)[0] == E_NOPL, "FRAME_GET -> GPU_ERR_NOPL")
        c.close()
    finally:
        d.stop()
    d = Daemon(["--sim", "--sim-pl-delay-ms", "1200"], "late-pl")
    try:
        c = Conn(d.port)
        check(c.hello()[0] == E_NOPL, "before the PL is configured: GPU_ERR_NOPL")
        t0 = time.time()
        while c.hello()[0] != OK and time.time() - t0 < T(8000) / 1000:
            time.sleep(0.1)
        dt = time.time() - t0
        check(c.hello()[0] == OK, f"PL picked up by the periodic/HELLO re-probe after {dt:.1f} s")
        fc = c.regs()[R["FRAME_COUNT"]]
        c.wait_frame(1, T(2000))
        st, w, h, fb = c.readback()
        check(st == OK and fb == struct.pack("<H", 0x0010) * (W * H), "start-up frame rendered once the PL appeared",
              f"fc {fc}")
        c.close()
    finally:
        d.stop()


# ---- hardware backend against fake /dev/mem files ------------------------------------------
def make_devmem(path, pages, pcfg_done=True, lvl=0xF, alias_id=None, regs=None, ddr=None):
    """pages: 2 = devcfg+SLCR, 3 = + GP0 page at 0x40000000, 4+ = + GP0 regs + 32 MB DDR"""
    size = 0x1000 * pages if pages < 4 else 0x4000 + 0x2000000
    buf = bytearray(min(size, 0x4000))
    struct.pack_into("<I", buf, 0x0C, 0x4 if pcfg_done else 0x0)       # devcfg INT_STS
    struct.pack_into("<I", buf, 0x1000 + 0x900, lvl)                    # SLCR LVL_SHFTR_EN
    if pages >= 3 and alias_id is not None:
        struct.pack_into("<I", buf, 0x2000, alias_id)
    if pages >= 4 and regs:
        for off, v in regs.items():
            struct.pack_into("<I", buf, 0x3000 + off, v)
    with open(path, "wb") as f:
        f.write(buf)
        if pages >= 4:
            f.truncate(size)
            if ddr:
                for addr, data in ddr:
                    f.seek(0x4000 + addr - DDR_BASE)
                    f.write(data)


def read_file(path, off, n):
    with open(path, "rb") as f:
        f.seek(off)
        return f.read(n)


# /proc/iomem of the board (512 MB, one x16 MT41K256M16): Linux has 0x00000000..0x0FFFFFFF; the matrix
# engine's 0x10000000..0x1DFFFFFF and the GPU window 0x1E000000..0x1FFFFFFF are reserved no-map, so
# Linux lists them outside "System RAM"
IOMEM_BOARD = ("00000000-0fffffff : System RAM\n"
               "  00008000-00bfffff : Kernel code\n"
               "  00d00000-00e7ffff : Kernel data\n"
               "40000000-40000fff : 40000000.pl_regs pl_regs@40000000\n"
               "e0000000-e0000fff : e0000000.serial serial@e0000000\n"
               "f8007000-f80070ff : f8007000.devcfg devcfg@f8007000\n")
IOMEM_FILES = {}


def iomem_file(tag, text):
    """write text to a fake /proc/iomem file in the temp dir; returns its path"""
    path = os.path.join(TMP, f"iomem-{tag}")
    with open(path, "w") as f:
        f.write(text)
    IOMEM_FILES[tag] = path
    return path


def test_hw_backend():
    print("[hardware backend: probe gating and register/DDR paths on fake /dev/mem files]")
    ftmp = tempfile.mkdtemp(prefix="fpgagpud-devmem-")
    try:
        cases = [
            ("PCFG_DONE=0", dict(pages=2, pcfg_done=False), "PL not configured"),
            ("level shifters off", dict(pages=2, lvl=0xA), "level shifters are off"),
            # platform bitstream without the GPU: pl_regs (ID 0x5A702001) answers at 0x40000000,
            # nothing at 0x43C00000 (decode error -> the probe child dies). Under qemu-arm (--slow)
            # the dying child can take longer than the 1 s probe limit (qemu writes its crash
            # report), so the daemon may report it as a hang instead; both refuse the address.
            ("platform bitstream without GPU", dict(pages=3, alias_id=PLATFORM_ID),
             "does not answer there" if ARGS.slow else "faulted"),
            # pz7020_ps7_top: pl_regs decodes only the low address bits, so 0x43C00000 aliases its ID
            ("pl_regs alias at 0x43C00000", dict(pages=4, alias_id=PLATFORM_ID, regs={0x000: PLATFORM_ID}),
             "not the FPGA-GPU"),
            # the same qemu-arm effect as above: the faulting probe child can outlast the 1 s limit,
            # so under --slow either report is the correct refusal (measured: 4 of 5 runs said hang)
            ("GP0 faults", dict(pages=2), ("faulted", "does not answer there") if ARGS.slow else "faulted"),
        ]
        for name, kw, needle in cases:
            path = os.path.join(ftmp, name.replace(" ", "_") + ".bin")
            make_devmem(path, **kw)
            d = Daemon(["--fake-devmem", path], "hw-" + name.replace(" ", "_").replace("=", ""))
            try:
                c = Conn(d.port)
                h1 = c.hello()
                time.sleep(2.3 if not ARGS.slow else 4.5)        # at least one periodic re-probe
                h2 = c.hello()
                st = c.status()
                c.close()
                log = d.logtext()
                check(h1[0] == E_NOPL and h2[0] == E_NOPL and st[0] == E_NOPL and d.alive() and (any(n in log for n in needle) if isinstance(needle, tuple) else needle in log),
                      f"{name}: GPU_ERR_NOPL, daemon alive (GP0 never touched), reason logged",
                      f"{h1[0]} {h2[0]} alive={d.alive()} log:{log[-300:]!r}")
            finally:
                d.stop()

        # U-Boot loaded pl.bit: Linux's zynq-fpga probe cleared PCFG_DONE, boot.scr put the marker
        # on the kernel command line
        gregs = {0x000: GPU_ID, 0x004: GPU_VERSION, 0x008: 0x4, 0x014: 0x1E000000, 0x018: 0x1E200000,
                 0x024: 512, 0x04C: 0x1FE00000}
        base_cmd = "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0"
        cmdfiles = {}
        for tag, text in (("yes", base_cmd + " fpgagpu.pl_loaded=1\n"), ("no", base_cmd + "\n"),
                          ("near", base_cmd + " fpgagpu.pl_loaded=10 xfpgagpu.pl_loaded=1\n")):
            cmdfiles[tag] = os.path.join(ftmp, "cmdline-" + tag)
            with open(cmdfiles[tag], "w") as f:
                f.write(text)
        for tag, ok, what in (("no", False, "no boot.scr marker"), ("near", False, "only look-alike markers")):
            path = os.path.join(ftmp, f"uboot-{tag}.bin")
            make_devmem(path, 4, pcfg_done=False, alias_id=GPU_ID, regs=gregs)
            d = Daemon(["--fake-devmem", path, "--cmdline", cmdfiles[tag]], "hw-uboot-" + tag)
            try:
                c = Conn(d.port)
                h = c.hello()
                c.close()
                r = struct.unpack("<24I", read_file(path, 0x3000, 96))
                check(h[0] == E_NOPL and r[0x08 // 4] == 0x4 and "fpgagpu.pl_loaded=1" in d.logtext(),
                      f"PCFG_DONE=0 and {what}: GPU_ERR_NOPL, GP0 untouched", f"{h[0]} control {r[2]:#x}")
            finally:
                d.stop()
        # the repo platform's boot: U-Boot loads the merged bitstream (pl_regs at 0x40000000, the
        # GPU at 0x43C00000), Linux cleared PCFG_DONE, /proc/iomem shows the GPU window as no-map
        path = os.path.join(ftmp, "uboot.bin")
        make_devmem(path, 4, pcfg_done=False, alias_id=PLATFORM_ID, regs=gregs)
        d = Daemon(["--fake-devmem", path, "--cmdline", cmdfiles["yes"], "--iomem", iomem_file("platform", IOMEM_BOARD)],
                   "hw-uboot")
        try:
            c = Conn(d.port)
            h = c.hello()
            r = struct.unpack("<24I", read_file(path, 0x3000, 96))
            check(h[0] == OK and h[2] == GPU_ID and r[0x08 // 4] == 0xD and "platform register file" in d.logtext(),
                  "platform boot (pl_regs at 0x40000000, PCFG_DONE=0 + fpgagpu.pl_loaded=1, window reserved): "
                  "HELLO ok, PL initialised", f"{h} control {r[2]:#x}")
            # the PL is reset after boot (INIT_B low -> devcfg INT_STS PCFG_INIT_NE): lost at once
            with open(path, "r+b") as f:
                f.seek(0x0C)
                f.write(struct.pack("<I", 0x1))
                f.seek(0x3000 + 0x104)
                f.write(struct.pack("<I", 0xAAAAAAAA))              # sentinel in PS_FIFO_SOR
            time.sleep(0.3)
            h = c.hello()
            c.send(RECT, struct.pack("<iiiiIfI", 0, 0, 16, 16, 0xFFFF, 0.0, 0))
            c.send(END_FRAME, struct.pack("<I", 3))
            sy = c.sync()
            sor = struct.unpack("<I", read_file(path, 0x3000 + 0x104, 4))[0]
            check(h[0] == E_NOPL and sy == (E_NOPL, 2) and sor == 0xAAAAAAAA and "PL lost" in d.logtext(),
                  "PL reset after boot (PCFG_INIT_NE): PL lost, drawing dropped, GP0 no longer written",
                  f"{h[0]} {sy} sor {sor:#x}")
            c.close()
            check(d.alive(), "daemon alive after the PL was lost")
        finally:
            d.stop()
        # marker present but the PL was already reset when the daemon starts
        path = os.path.join(ftmp, "uboot-reset.bin")
        make_devmem(path, 4, pcfg_done=False, alias_id=GPU_ID, regs=gregs)
        with open(path, "r+b") as f:
            f.seek(0x0C)
            f.write(struct.pack("<I", 0x1))
        d = Daemon(["--fake-devmem", path, "--cmdline", cmdfiles["yes"]], "hw-uboot-reset")
        try:
            c = Conn(d.port)
            h = c.hello()
            c.close()
            check(h[0] == E_NOPL and "PL was reset after U-Boot loaded pl.bit" in d.logtext(),
                  "marker + PCFG_INIT_NE already set: GPU_ERR_NOPL, reason logged", str(h[0]))
        finally:
            d.stop()

        # a complete fake GPU: registers + DDR window
        path = os.path.join(ftmp, "gpu.bin")
        pattern = bytes((i * 7 + 3) & 255 for i in range(FB_BYTES))
        regs = {0x000: GPU_ID, 0x004: GPU_VERSION, 0x008: 0x4, 0x010: 5, 0x014: 0x1E000000, 0x018: 0x1E200000,
                0x01C: 0, 0x024: 512, 0x04C: 0x1FE00000}
        make_devmem(path, 4, alias_id=GPU_ID, regs=regs, ddr=[(0x1E000000, pattern)])
        d = Daemon(["--fake-devmem", path], "hw-gpu")
        try:
            c = Conn(d.port)
            h = c.hello()
            check(h[0] == OK and h[2] == GPU_ID and h[3] == GPU_VERSION, "fake GPU: HELLO ok with ID/VERSION", str(h))
            r = struct.unpack("<24I", read_file(path, 0x3000, 96))
            check(r[0x20 // 4] == 0x0010 and r[0x08 // 4] == 0xD and r[0x50 // 4] == 2,
                  "PL init wrote CLEAR_COLOR=0x0010, CONTROL=SOFT_RESET|SRC_PS|SCANOUT_EN, RET_CTRL=ACK",
                  f"clear {r[8]:#x} control {r[2]:#x} ret_ctrl {r[20]:#x}")
            sor, data = struct.unpack("<II", read_file(path, 0x3000 + 0x104, 4) + read_file(path, 0x3000 + 0x100, 4))
            check(sor == 0xF0000000 and data == 0, "start-up END record written to PS_FIFO_SOR / PS_FIFO_DATA")
            st, w, hh, fb = c.readback()
            check(st == OK and fb == pattern, "READBACK reads FB0 through the DDR window (64-bit copies)")
            pix = sprite_pixels(5, 16, 4)
            st, addr, stride = c.upload(3, 16, 4, pix)
            check(st == OK and addr == POOL_ADDR and read_file(path, 0x4000 + POOL_ADDR - DDR_BASE, len(pix)) == pix,
                  "SPRITE_UPLOAD wrote the pixels into the DDR window")
            c.send(RECT, struct.pack("<iiiiIfI", 0, 0, 16, 16, 0xFFFF, 0.0, 0))
            c.send(END_FRAME, struct.pack("<I", 0x12345678))
            check(c.sync() == (OK, 0), "drawing pushed through the register window")
            sor, data = struct.unpack("<II", read_file(path, 0x3000 + 0x104, 4) + read_file(path, 0x3000 + 0x100, 4))
            check(sor == 0xF0000000 and data == 0, "last pushed record was the END")
            check(c.wait_frame(6, 100) == (E_TIMEOUT, 5), "WAIT_FRAME reads FRAME_COUNT from the register")
            check(c.status()[1][R["FRAME_COUNT"]] == 5 and c.status()[3] >= 3, "STATUS reads the register window")
            c.close()
            check(d.alive(), "fake GPU: daemon alive")
        finally:
            d.stop()

        # the DDR window must be reserved (no-map) before GP0 is touched: on the board (512 MB) the window is
        # the top 32 MB of RAM, so without the device tree's reservation Linux would own it
        wcases = [
            ("window reserved (the board: 512 MB, engine + GPU reserved)", IOMEM_BOARD, True, ""),
            ("window reserved (Linux up to 0x1DFFFFFF, no engine window)", "00000000-1dffffff : System RAM\n", True, ""),
            ("no reserved-memory node (all 512 MB is System RAM)",
             "00000000-1fffffff : System RAM\n  00008000-00bfffff : Kernel code\n", False, "is Linux RAM"),
            ("window partly System RAM", "00000000-1dffffff : System RAM\n1f000000-3fffffff : System RAM\n",
             False, "is Linux RAM"),
            ("iomem without addresses (not root)", "00000000-00000000 : System RAM\n00000000-00000000 : System RAM\n",
             False, "cannot verify"),
            ("iomem unreadable", None, False, "cannot read"),
        ]
        for i, (name, text, ok, needle) in enumerate(wcases):
            path = os.path.join(ftmp, f"window-{i}.bin")
            make_devmem(path, 4, alias_id=PLATFORM_ID, regs=gregs)
            ipath = iomem_file(f"w{i}", text) if text is not None else os.path.join(ftmp, "no-such-iomem")
            d = Daemon(["--fake-devmem", path, "--iomem", ipath], f"hw-window-{i}")
            try:
                c = Conn(d.port)
                h = c.hello()
                c.close()
                r = struct.unpack("<24I", read_file(path, 0x3000, 96))
                if ok:
                    check(h[0] == OK and r[0x08 // 4] == 0xD and "DDR window 0x1e000000+0x2000000 reserved" in d.logtext(),
                          f"{name}: HELLO ok, PL initialised", f"{h[0]} control {r[2]:#x} log:{d.logtext()[-300:]!r}")
                else:
                    check(h[0] == E_NOPL and r[0x08 // 4] == 0x4 and needle in d.logtext() and d.alive(),
                          f"{name}: GPU_ERR_NOPL, GP0 untouched, reason logged",
                          f"{h[0]} control {r[2]:#x} log:{d.logtext()[-300:]!r}")
            finally:
                d.stop()
    finally:
        shutil.rmtree(ftmp, ignore_errors=True)


def test_throughput(d):
    print("[throughput (informational)]")
    ctl = Conn(d.port)
    ctl.hello()
    ctl.set_config(CTL_P | CTL_S, 0)
    rng = random.Random(5)
    frames = 30 if not ARGS.slow else 5
    sc = Scene()
    random_scene(rng, sc, 200, 0)
    fc = ctl.regs()[R["FRAME_COUNT"]]
    t0 = time.time()
    for i in range(frames):
        for t, p in sc.msgs:
            ctl.send(t, p)
        ctl.send(END_FRAME, struct.pack("<I", i))
    st, fc2 = ctl.wait_frame(fc + frames, T(60000))
    dt = time.time() - t0
    check(st == OK, f"{frames} frames x 200 tris: {frames / dt:.1f} frames/s, "
          f"{frames * 200 / dt:.0f} tris/s (sim incl. software rendering)")
    ctl.close()


def main():
    global ARGS, TMP
    ap = argparse.ArgumentParser()
    ap.add_argument("--daemon", default="build/fpgagpud_sim")
    ap.add_argument("--ref", default="build/test_ref")
    ap.add_argument("--slow", action="store_true", help="longer timeouts (ARM build under qemu-arm)")
    ap.add_argument("--keep", action="store_true", help="keep the temp dir with logs")
    ARGS = ap.parse_args()
    TMP = tempfile.mkdtemp(prefix="fpgagpud-test-")
    t0 = time.time()
    print(f"fpgagpud tests: daemon {ARGS.daemon}, reference {ARGS.ref}, temp {TMP}")
    d = None
    try:
        d = Daemon(["--sim"], "sim")
        for fn in (test_basic_and_render, test_errors, test_wait_and_frame_get, test_teensy_barrier,
                   test_throughput):
            try:
                fn(d)
            except Exception as e:                          # noqa: BLE001
                check(False, f"{fn.__name__} raised", repr(e))
        check(d.alive(), "main daemon still running after all tests")
        d.stop()
        for fn in (test_nopl, test_hw_backend):
            try:
                fn()
            except Exception as e:                          # noqa: BLE001
                check(False, f"{fn.__name__} raised", repr(e))
    finally:
        for dd in LOGS:
            dd.stop()
    print(f"\n{RESULTS['pass']} passed, {RESULTS['fail']} failed in {time.time() - t0:.1f} s")
    if RESULTS["fail"]:
        for dd in LOGS:
            print(f"--- tail of {dd.logpath}")
            print(dd.logtext()[-3000:])
    if not ARGS.keep and not RESULTS["fail"]:
        shutil.rmtree(TMP, ignore_errors=True)
    sys.exit(1 if RESULTS["fail"] else 0)


if __name__ == "__main__":
    main()
