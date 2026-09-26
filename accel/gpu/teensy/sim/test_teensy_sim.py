#!/usr/bin/env python3
"""
test_teensy_sim.py -- end-to-end tests of the Teensy simulator (python3 standard library only).

Starts build/teensy_sim on free ports and talks to it exactly like the Pi does (T_* protocol of
common/gpu_proto.h over TCP instead of USB CDC). The simulated parallel bus is a fake FPGA bus
server in this script (it records every byte the "Teensy" sends and can stall like a full FIFO,
disappear like a powered-down FPGA, and come back). Expected records come from build/geom_ref:
plain geom_mesh_upload / geom_frame calls with none of the firmware code in between.

    python3 test_teensy_sim.py --sim build/teensy_sim --ref build/geom_ref [--daemon PATH] [--slow]

--daemon: if that fpgagpud_sim binary exists, an extra end-to-end part runs the simulator against
the real daemon simulator's Teensy bus (T_FRAME -> rendered frame read back bit-exact against
gpu_refrast of geom_ref's records; autonomous mode -> daemon FRAME_COUNT / LAST_FRAME_NO advance,
no bad records). Exit status 0 = all checks passed.
"""
import argparse
import math
import os
import random
import select
import socket
import struct
import subprocess
import sys
import threading
import time

TMAGIC, NMAGIC, REPLY = 0x31534754, 0x31504746, 0x8000
T_HELLO, T_MESH, T_FRAME, T_RECORDS, T_STATS, T_BUS_MODE, T_RESET, T_SCENE, T_AUTO = 1, 2, 3, 4, 6, 7, 8, 9, 10
OK, E_PROTO, E_ARG, E_NOMEM, E_BUS = 0, -1, -3, -4, -6
REC = 96
TRI, SPRITE, END = 1, 2, 15
F_RETURN, F_NO_BUS = 1, 2
D_ZTEST, D_ZWRITE, D_CULL, D_LIGHT = 1, 2, 4, 8
F_NOEDGE = 1 << 27
W, H = 1280, 720
STATS_FIELDS = ("status frames records_sent words_sent bus_timeouts fpga_ready bus_enabled usb_bad_msgs "
                "auto_running auto_frames auto_fps_x100 cpu_busy_pct tris_per_sec cpu_mhz").split()

ARGS = None
SLOW = 1.0
RESULTS = {"pass": 0, "fail": 0}


def check(cond, name, detail=""):
    if cond:
        RESULTS["pass"] += 1
        print(f"  PASS  {name}")
    else:
        RESULTS["fail"] += 1
        print(f"  FAIL  {name}  {detail}")
    return cond


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ------------------------------------------------------------------------------------------------
# float32 emulation (exact: double has > 2*24+2 bits, so rounding each double op to float32 equals
# the float32 op) -- used to predict the firmware's viewproj = proj * view in autonomous mode
# ------------------------------------------------------------------------------------------------
def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def mat_mul_f32(a, b):
    """o = a * b, column-major, SPEC 6 summation order, every op rounded to float32"""
    o = [0.0] * 16
    for c in range(4):
        for r in range(4):
            s = f32(f32(a[0 * 4 + r] * b[c * 4 + 0]) + f32(a[1 * 4 + r] * b[c * 4 + 1]))
            s = f32(s + f32(a[2 * 4 + r] * b[c * 4 + 2]))
            o[c * 4 + r] = f32(s + f32(a[3 * 4 + r] * b[c * 4 + 3]))
    return o


def as_f32(m):
    return [f32(v) for v in m]


def mat_mul(a, b):
    o = [0.0] * 16
    for c in range(4):
        for r in range(4):
            o[c * 4 + r] = sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
    return o


def perspective(fovy, aspect, near, far):
    f = 1.0 / math.tan(fovy / 2)
    m = [0.0] * 16
    m[0] = f / aspect
    m[5] = f
    m[10] = (far + near) / (near - far)
    m[11] = -1.0
    m[14] = 2 * far * near / (near - far)
    return m


def look_at(eye, center, up):
    fx, fy, fz = (center[i] - eye[i] for i in range(3))
    n = math.sqrt(fx * fx + fy * fy + fz * fz)
    fx, fy, fz = fx / n, fy / n, fz / n
    sx, sy, sz = fy * up[2] - fz * up[1], fz * up[0] - fx * up[2], fx * up[1] - fy * up[0]
    n = math.sqrt(sx * sx + sy * sy + sz * sz)
    sx, sy, sz = sx / n, sy / n, sz / n
    ux, uy, uz = sy * fz - sz * fy, sz * fx - sx * fz, sx * fy - sy * fx
    m = [sx, ux, -fx, 0, sy, uy, -fy, 0, sz, uz, -fz, 0, 0, 0, 0, 1]
    m[12] = -(sx * eye[0] + sy * eye[1] + sz * eye[2])
    m[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2])
    m[14] = fx * eye[0] + fy * eye[1] + fz * eye[2]
    return m


def model_matrix(tx, ty, tz, ay=0.0, ax=0.0, s=1.0):
    cy, sy_, cx, sx = math.cos(ay), math.sin(ay), math.cos(ax), math.sin(ax)
    ry = [cy, 0, -sy_, 0, 0, 1, 0, 0, sy_, 0, cy, 0, 0, 0, 0, 1]
    rx = [1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1]
    m = mat_mul(ry, rx)
    m = [v * s for v in m[:12]] + [0, 0, 0, 1]
    m[12], m[13], m[14] = tx, ty, tz
    return m


# ------------------------------------------------------------------------------------------------
# meshes and message payloads
# ------------------------------------------------------------------------------------------------
def vtx(p, n, c):
    return struct.pack("<6f4B", p[0], p[1], p[2], n[0], n[1], n[2], c[0], c[1], c[2], 255)


def mesh_payload(mesh_id, verts, idx):
    return struct.pack("<HHII", mesh_id, 0, len(verts), len(idx)) + b"".join(verts) + struct.pack(f"<{len(idx)}H", *idx)


def cube_mesh():
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    cols = [(255, 40, 40), (40, 255, 40), (40, 40, 255), (255, 255, 40), (255, 40, 255), (40, 255, 255)]
    verts, idx = [], []
    for fi, (n, u, v) in enumerate(faces):
        base = len(verts)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):   # CCW seen from outside (u x v = n)
            p = tuple(n[k] + su * u[k] + sv * v[k] for k in range(3))
            verts.append(vtx(p, n, cols[fi]))
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return verts, idx


def sphere_mesh(nlat=12, nlon=16):
    verts, idx = [], []
    for i in range(nlat + 1):
        th = math.pi * i / nlat
        for j in range(nlon + 1):
            ph = 2 * math.pi * j / nlon
            n = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
            c = (int(127 + 120 * n[0]), int(127 + 120 * n[1]), int(127 + 120 * n[2]))
            verts.append(vtx(n, n, c))
    for i in range(nlat):
        for j in range(nlon):
            a, b = i * (nlon + 1) + j, (i + 1) * (nlon + 1) + j
            idx += [a, a + 1, b, b, a + 1, b + 1]
    return verts, idx


def grid_mesh(n, rnd):
    verts, idx = [], []
    for i in range(n + 1):
        for j in range(n + 1):
            p = (-1 + 2 * j / n, 0.05 * math.sin(i * 0.7) * math.cos(j * 0.5), -1 + 2 * i / n)
            verts.append(vtx(p, (0, 1, 0), (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256))))
    for i in range(n):
        for j in range(n):
            a, b = i * (n + 1) + j, (i + 1) * (n + 1) + j
            idx += [a, b, a + 1, a + 1, b, b + 1]
    return verts, idx


def draw(mesh_id, flags, model, mul=(255, 255, 255)):
    return struct.pack("<HH16f4B", mesh_id, flags, *model, mul[0], mul[1], mul[2], 0)


def frame_payload(frame_no, vp, draws, flags=0, light=(0.3, -0.8, -0.52), ambient=0.25):
    return struct.pack("<I16f3ffHH", frame_no, *vp, *light, ambient, len(draws), flags) + b"".join(draws)


def scene_payload(view, proj, objs, overlays, light=(0.3, -0.8, -0.52), ambient=0.25, orbit=0.0):
    return (struct.pack("<II16f16f3fff", len(objs), len(overlays), *view, *proj, *light, ambient, orbit)
            + b"".join(objs) + b"".join(overlays))


def scene_obj(mesh_id, flags, model, axis=(0, 1, 0), rate=0.0, mul=(255, 255, 255)):
    return struct.pack("<HH16f3ff4B", mesh_id, flags, *model, *axis, rate, mul[0], mul[1], mul[2], 0)


def rect_rec(x0, y0, x1, y1, r, g, b):
    w = [0] * 24
    w[0] = (TRI << 28) | F_NOEDGE | ((x1 - 1) << 11) | x0
    w[1] = ((y1 - 1) << 11) | y0
    w[14], w[17], w[20] = r << 16, g << 16, b << 16
    return struct.pack("<24I", *w)


def sprite_rec(x, y, w_, h_, src, stride):
    w = [0] * 24
    w[0] = SPRITE << 28
    w[1], w[2], w[3], w[4] = (y << 11) | x, (h_ << 11) | w_, src, stride
    return struct.pack("<24I", *w)


def rec_type(buf, i):
    return buf[i * REC + 3] >> 4


def split_frames(buf):
    """-> list of (frame_no, records bytes incl. END); trailing incomplete frame dropped"""
    n = len(buf) // REC
    frames, start = [], 0
    for i in range(n):
        if rec_type(buf, i) == END:
            fno = struct.unpack_from("<I", buf, i * REC + 4)[0]
            frames.append((fno, bytes(buf[start * REC:(i + 1) * REC])))
            start = i + 1
    return frames


# ------------------------------------------------------------------------------------------------
# fake FPGA bus (the daemon simulator's Teensy bus port)
# ------------------------------------------------------------------------------------------------
class FakeBus:
    def __init__(self, port=0):
        self.port = port
        self.lock = threading.Lock()
        self.data = bytearray()
        self.conns = 0
        self.paused = False
        self.stop_flag = False
        self.lsock = None
        self.csock = None
        self.thread = None

    def start(self):
        self.lsock = socket.socket()
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # small receive window from the SYN on (inherited by accepted sockets): when paused, the
        # sender's buffers fill quickly and stay full, like a full FIFO holding BUSY = 1
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
        self.lsock.bind(("127.0.0.1", self.port))
        self.port = self.lsock.getsockname()[1]
        self.lsock.listen(4)
        self.stop_flag = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        return self

    def _run(self):
        while not self.stop_flag:
            socks = [self.lsock] + ([self.csock] if self.csock and not self.paused else [])
            try:
                r, _, _ = select.select(socks, [], [], 0.02)
            except (OSError, ValueError):
                break
            for s in r:
                if s is self.lsock:
                    c, _ = self.lsock.accept()
                    if self.csock:
                        self.csock.close()
                    self.csock = c
                    self.conns += 1
                else:
                    try:
                        d = s.recv(1 << 20)
                    except OSError:
                        d = b""
                    if not d:
                        s.close()
                        self.csock = None
                    else:
                        with self.lock:
                            self.data += d

    def stop(self):
        self.stop_flag = True
        self.thread.join()
        if self.csock:
            self.csock.close()
            self.csock = None
        self.lsock.close()

    def take(self):
        with self.lock:
            d = bytes(self.data)
            self.data.clear()
        return d

    def size(self):
        with self.lock:
            return len(self.data)

    def wait_bytes(self, n, timeout=3.0):
        t = time.time() + timeout * SLOW
        while time.time() < t:
            if self.size() >= n:
                return True
            time.sleep(0.005)
        return self.size() >= n


# ------------------------------------------------------------------------------------------------
# clients
# ------------------------------------------------------------------------------------------------
def recv_exact(s, n):
    b = bytearray()
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise ConnectionError("connection closed")
        b += d
    return bytes(b)


class Link:
    def __init__(self, port, magic, timeout=5.0):
        self.magic = magic
        t = time.time() + 5 * SLOW
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                if time.time() > t:
                    raise
                time.sleep(0.05)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(timeout * SLOW)

    def send(self, typ, payload=b""):
        self.s.sendall(struct.pack("<IHHI", self.magic, typ, 0, len(payload)) + payload)

    def raw(self, b):
        self.s.sendall(b)

    def recv(self, typ):
        magic, rtyp, flags, ln = struct.unpack("<IHHI", recv_exact(self.s, 12))
        if magic != self.magic or rtyp != (typ | REPLY) or flags != 0:
            raise AssertionError(f"bad reply header {magic:#x} {rtyp:#x} {flags} (wanted {typ | REPLY:#x})")
        return recv_exact(self.s, ln)

    def call(self, typ, payload=b""):
        self.send(typ, payload)
        return self.recv(typ)

    def close(self):
        self.s.close()


class Teensy(Link):
    def __init__(self, port):
        super().__init__(port, TMAGIC)

    def status(self, typ, payload=b""):
        return struct.unpack_from("<i", self.call(typ, payload))[0]

    def hello(self):
        return dict(zip("status fw max_meshes max_verts max_indices fpga_ready bus_enabled".split(),
                        struct.unpack("<i6I", self.call(T_HELLO))))

    def stats(self):
        r = self.call(T_STATS)
        return dict(zip(STATS_FIELDS, struct.unpack("<i13I", r)))

    def frame(self, payload):
        """-> (reply dict, returned records bytes)"""
        r = self.call(T_FRAME, payload)
        v = struct.unpack_from("<i8I", r)
        d = dict(zip("status frame_no tris_in tris_out tris_culled tris_clipped us_total us_bus_wait nrecs".split(), v))
        return d, r[36:], len(r)

    def wait_ready(self, want=1, timeout=3.0):
        t = time.time() + timeout * SLOW
        while time.time() < t:
            h = self.hello()
            if h["fpga_ready"] == want and h["bus_enabled"] == want:
                return True
            time.sleep(0.02)
        return False


class Ref:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def _cmd(self, op, payload):
        self.p.stdin.write(struct.pack("<II", op, len(payload)) + payload)
        self.p.stdin.flush()

    def mesh(self, payload):
        self._cmd(1, payload)
        return struct.unpack("<i", self.p.stdout.read(4))[0]

    def frame(self, payload):
        self._cmd(2, payload)
        st, n, ti, to, tc, tcl = struct.unpack("<i5I", self.p.stdout.read(24))
        recs = self.p.stdout.read(n * REC)
        return st, recs, dict(tris_in=ti, tris_out=to, tris_culled=tc, tris_clipped=tcl)

    def render(self, clear, recs):
        self._cmd(3, struct.pack("<I", clear) + recs)
        return self.p.stdout.read(W * H * 2)

    def reset(self):
        self._cmd(4, b"")
        return struct.unpack("<i", self.p.stdout.read(4))[0]

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)


def start_sim(port, bus_port, name, extra=()):
    log = open(os.path.join(ARGS.logdir, name), "w")
    p = subprocess.Popen([ARGS.sim, "--port", str(port), "--bus-port", str(bus_port), "-v", *extra],
                         stdout=log, stderr=subprocess.STDOUT)
    return p, log


def stop_proc(p, name):
    p.terminate()
    try:
        rc = p.wait(timeout=10 * SLOW)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = p.wait()
    check(rc == 0, f"{name} exits cleanly on SIGTERM", f"rc={rc}")


# ------------------------------------------------------------------------------------------------
# tests
# ------------------------------------------------------------------------------------------------
def test_sim():
    rnd = random.Random(1234)
    bus_port = free_port()
    port = free_port()
    sim, log = start_sim(port, bus_port, "teensy_sim.log")
    ref = Ref(ARGS.ref)
    try:
        t = Teensy(port)
        print("[hello / no FPGA]")
        h = t.hello()
        check(h["status"] == 0 and h["fw"] == 1 and h["max_meshes"] == 16 and h["max_verts"] == 8192
              and h["max_indices"] == 24576, "T_HELLO fields", str(h))
        check(h["fpga_ready"] == 0 and h["bus_enabled"] == 0, "no bus: fpga_ready = bus_enabled = 0", str(h))

        cube_v, cube_i = cube_mesh()
        sph_v, sph_i = sphere_mesh()
        grid_v, grid_i = grid_mesh(60, rnd)
        meshes = {1: mesh_payload(1, cube_v, cube_i), 2: mesh_payload(2, sph_v, sph_i),
                  7: mesh_payload(7, grid_v, grid_i)}
        print("[T_MESH]")
        for mid, pl in meshes.items():
            st = t.status(T_MESH, pl)
            check(st == OK and ref.mesh(pl) == OK, f"mesh {mid} upload ({len(pl)} bytes)", f"st={st}")
        bad = mesh_payload(9, cube_v, cube_i[:-1])                     # nidx % 3 != 0
        check(t.status(T_MESH, bad) == E_ARG, "mesh nidx % 3 != 0 -> GPU_ERR_ARG")
        bad = mesh_payload(9, cube_v[:4], [0, 1, 4])                   # index out of range
        check(t.status(T_MESH, bad) == E_ARG, "mesh index >= nverts -> GPU_ERR_ARG")
        bad = struct.pack("<HHII", 9, 0, 3, 3) + b"\0" * 10            # header says more than len
        check(t.status(T_MESH, bad) == E_PROTO, "mesh length mismatch -> GPU_ERR_PROTO")
        big = struct.pack("<HHII", 9, 0, 9000, 0) + b"\0" * (9000 * 28)
        check(t.status(T_MESH, big) == E_NOMEM, "mesh > GEOM_MAX_VERTS -> GPU_ERR_NOMEM")

        proj = perspective(math.radians(60), 16 / 9, 0.1, 100.0)
        view = look_at((0.3, 1.6, 4.5), (0, 0, 0), (0, 1, 0))
        vp = mat_mul(proj, view)
        draws = [draw(1, D_ZTEST | D_ZWRITE | D_CULL | D_LIGHT, model_matrix(-1.4, 0.2, 0, 0.6, 0.3, 0.8)),
                 draw(2, D_ZTEST | D_ZWRITE | D_LIGHT, model_matrix(1.3, 0.0, 0.5, 0.2, 0.0, 1.0), (255, 200, 120)),
                 draw(7, D_ZTEST | D_ZWRITE, model_matrix(0, -1.0, 0, 0.3, 0.0, 6.0)),     # clipped by near/sides
                 draw(1, D_ZTEST | D_ZWRITE, model_matrix(0.0, 0.3, 3.9, 0.4, 0.2, 1.0))]  # camera inside-ish: near clip

        print("[T_FRAME without the bus]")
        pl = frame_payload(41, vp, draws, F_RETURN | F_NO_BUS)
        rst, rrecs, rstats = ref.frame(pl)
        d, recs, _ = t.frame(pl)
        check(d["status"] == 0 and rst == 0, "RETURN|NO_BUS works with no FPGA", str(d))
        check(recs == rrecs and d["nrecs"] == len(rrecs) // REC,
              f"RETURN|NO_BUS records == geom_frame ({len(rrecs) // REC} records)")
        check(all(d[k] == rstats[k] for k in rstats) and rstats["tris_clipped"] > 0 and rstats["tris_culled"] > 0,
              "frame stats == geom_frame stats (with clipped and culled triangles)", f"{d} {rstats}")
        t0 = time.time()
        d, recs, ln = t.frame(frame_payload(42, vp, draws))
        el = time.time() - t0
        check(d["status"] == E_BUS and ln == 36 and d["nrecs"] == 0,
              "T_FRAME to the bus with no FPGA -> GPU_ERR_BUS", str(d))
        check(0.45 <= el <= 2.0 * SLOW, f"... after the 500 ms BUSY timeout ({el * 1000:.0f} ms)")
        s = t.stats()
        check(s["bus_timeouts"] == 1 and s["records_sent"] == 0 and s["frames"] == 1, "stats after the timeout", str(s))

        print("[bus appears (connect retry, 10 ms arming)]")
        bus = FakeBus(bus_port).start()
        check(t.wait_ready(1, 3.0), "fpga_ready = bus_enabled = 1 after the bus comes up")
        check(bus.conns == 1, "one bus connection", str(bus.conns))

        print("[T_FRAME to the bus]")
        pl = frame_payload(43, vp, draws)
        rst, rrecs, rstats = ref.frame(pl)
        bus.take()
        d, recs, ln = t.frame(pl)
        check(d["status"] == 0 and ln == 36 and d["frame_no"] == 43, "T_FRAME ok, plain 36-byte reply", str(d))
        check(bus.wait_bytes(len(rrecs)) and bus.take() == rrecs,
              f"bus bytes == geom_frame records ({len(rrecs) // REC} records, END last)")
        check(all(d[k] == rstats[k] for k in rstats), "reply stats == geom_frame stats")
        check(d["us_bus_wait"] <= d["us_total"], "us_bus_wait <= us_total", str(d))

        print("[T_FRAME with GEOM_FRAME_RETURN]")
        pl = frame_payload(44, vp, draws, F_RETURN)
        rst, rrecs, rstats = ref.frame(pl)
        d, recs, ln = t.frame(pl)
        check(d["status"] == 0 and d["nrecs"] == len(rrecs) // REC and ln == 36 + len(rrecs),
              f"RETURN reply: nrecs_returned {d['nrecs']} + records", str(d))
        check(recs == rrecs, "returned records == geom_frame records (TRI + END)")
        check(bus.wait_bytes(len(rrecs)) and bus.take() == rrecs, "the bus got the same records")
        rtypes = [rec_type(recs, i) for i in range(len(recs) // REC)]
        check(rtypes[-1] == END and all(x == TRI for x in rtypes[:-1]), "returned: TRIs then END")
        pl = frame_payload(45, vp, draws[:2], F_RETURN | F_NO_BUS)
        rst, rrecs, _ = ref.frame(pl)
        d, recs, ln = t.frame(pl)
        time.sleep(0.1)
        check(d["status"] == 0 and recs == rrecs and bus.size() == 0, "RETURN|NO_BUS: records back, bus untouched")
        d, recs, ln = t.frame(frame_payload(46, vp, [draw(5, 0, model_matrix(0, 0, 0))]))
        time.sleep(0.05)
        check(d["status"] == E_ARG and bus.size() == 0, "unknown mesh -> GPU_ERR_ARG, nothing on the bus")
        d, _, _ = t.frame(frame_payload(47, vp, []))
        check(d["status"] == 0 and bus.wait_bytes(REC) and bus.take() ==
              struct.pack("<24I", END << 28, 47, *([0] * 22)), "empty frame -> only END(47)")

        print("[T_RECORDS]")
        recs3 = b"".join(struct.pack("<24I", *[rnd.getrandbits(32) for _ in range(24)]) for _ in range(3))
        r = t.call(T_RECORDS, struct.pack("<I", 3) + recs3)
        check(struct.unpack("<ii", r) == (0, 3), "T_RECORDS reply (0, 3)", str(struct.unpack("<ii", r)))
        check(bus.wait_bytes(len(recs3)) and bus.take() == recs3, "raw records forwarded unchanged")
        r = t.call(T_RECORDS, struct.pack("<I", 2) + recs3)
        check(struct.unpack("<ii", r) == (E_PROTO, 0), "T_RECORDS count/len mismatch -> GPU_ERR_PROTO")

        print("[framing]")
        b0 = t.stats()["usb_bad_msgs"]
        t.raw(b"\x01\x02TG\xffxyz")                                      # junk
        check(t.hello()["status"] == 0, "HELLO after junk bytes")
        r = t.call(99, b"abcde")
        check(struct.unpack("<i", r)[0] == E_PROTO, "unknown type -> PROTO reply of that type, payload skipped")
        check(t.status(T_AUTO, b"\0" * 7) == E_PROTO, "T_AUTO wrong length -> GPU_ERR_PROTO")
        check(t.status(T_BUS_MODE, struct.pack("<I", 5)) == E_ARG, "T_BUS_MODE 5 -> GPU_ERR_ARG")
        check(t.hello()["status"] == 0, "stream still in sync")
        check(t.stats()["usb_bad_msgs"] >= b0 + 3, "usb_bad_msgs counted")

        print("[T_BUS_MODE]")
        check(t.status(T_BUS_MODE, struct.pack("<I", 1)) == OK, "bus mode 1 (force off)")
        h = t.hello()
        check(h["bus_enabled"] == 0 and h["fpga_ready"] == 1, "forced off: pins high-Z, FPGA still ready", str(h))
        t0 = time.time()
        d, _, _ = t.frame(frame_payload(48, vp, draws))
        check(d["status"] == E_BUS and time.time() - t0 < 0.3 * SLOW, "T_FRAME while forced off -> GPU_ERR_BUS at once")
        check(t.status(T_BUS_MODE, struct.pack("<I", 0)) == OK and t.wait_ready(1, 1.0), "bus mode 0 -> enabled again")
        bus.take()

        print("[autonomous mode, static scene: records predicted exactly]")
        check(t.status(T_AUTO, struct.pack("<II", 1, 0)) == E_ARG, "T_AUTO before any T_SCENE -> GPU_ERR_ARG")
        proj32, view32 = as_f32(proj), as_f32(view)
        m1, m2 = as_f32(model_matrix(-1.2, 0, 0, 0.5, 0.2)), as_f32(model_matrix(1.2, 0, 0, 0.1, 0.0, 0.9))
        objs = [scene_obj(1, D_ZTEST | D_ZWRITE | D_CULL | D_LIGHT, m1),
                scene_obj(3, D_ZTEST | D_ZWRITE, m1),                    # mesh 3 not uploaded: skipped
                scene_obj(2, D_ZTEST | D_ZWRITE | D_LIGHT, m2, mul=(200, 255, 90))]
        ovl = [sprite_rec(16, 16, 64, 32, 0x1E400000, 128), rect_rec(1200, 680, 1270, 710, 255, 255, 0)]
        bad_ovl = [struct.pack("<24I", END << 28, 1, *([0] * 22))]
        check(t.status(T_SCENE, scene_payload(view32, proj32, objs, bad_ovl)) == E_ARG,
              "T_SCENE with an END overlay -> GPU_ERR_ARG")
        check(t.status(T_SCENE, scene_payload(view32, proj32, objs, ovl)[:-1]) == E_PROTO,
              "T_SCENE length mismatch -> GPU_ERR_PROTO")
        check(t.status(T_SCENE, scene_payload(view32, proj32, objs, ovl)) == OK, "T_SCENE (3 objects, 2 overlays)")
        vp32 = mat_mul_f32(proj32, view32)
        _, pred, _ = ref.frame(frame_payload(0, vp32, [draw(1, D_ZTEST | D_ZWRITE | D_CULL | D_LIGHT, m1),
                                                      draw(2, D_ZTEST | D_ZWRITE | D_LIGHT, m2, (200, 255, 90))]))
        pred_tris = pred[:-REC]
        s0 = t.stats()
        bus.take()
        check(t.status(T_AUTO, struct.pack("<II", 1, 0)) == OK, "T_AUTO enable=1 max_fps=0")
        time.sleep(0.6 * SLOW)
        h = t.hello()
        s1 = t.stats()
        check(h["status"] == 0 and s1["auto_running"] == 1, "T_HELLO / T_STATS answered while running", str(s1))
        d, _, ln = t.frame(frame_payload(49, vp, draws))
        check(d["status"] == E_ARG and ln == 36, "T_FRAME while autonomous -> GPU_ERR_ARG")
        r = t.call(T_RECORDS, struct.pack("<I", 1) + recs3[:REC])
        check(struct.unpack("<ii", r) == (E_ARG, 0), "T_RECORDS while autonomous -> GPU_ERR_ARG")
        time.sleep(0.6 * SLOW)
        frames = split_frames(bus.take())
        s2 = t.stats()
        fnos = [f[0] for f in frames]
        check(len(frames) >= 20, f"autonomous frames produced: {len(frames)} in ~1.2 s")
        check(all(b == a + 1 for a, b in zip(fnos, fnos[1:])), "frame_no increases by 1 per frame",
              str(fnos[:5]))
        good = sum(1 for _, f in frames if f[:-3 * REC] == pred_tris and f[-3 * REC:-REC] == b"".join(ovl))
        check(good == len(frames), f"every frame = predicted TRIs + overlays + END ({good}/{len(frames)})")
        check(s2["auto_frames"] >= s0["auto_frames"] + len(frames) and s2["auto_frames"] >= fnos[-1],
              "auto_frames counts them", f"{s2['auto_frames']} {len(frames)} {fnos[-1]}")
        check(s2["cpu_mhz"] == 600 and 0 < s2["auto_fps_x100"] and 0 < s2["tris_per_sec"]
              and 0 <= s2["cpu_busy_pct"] <= 100, "T_STATS rates sane", str(s2))
        check(s2["words_sent"] == s2["records_sent"] * 24, "words_sent = 24 * records_sent")

        print("[autonomous: max_fps, T_SCENE while running]")
        check(t.status(T_AUTO, struct.pack("<II", 1, 25)) == OK, "T_AUTO max_fps=25 while running")
        time.sleep(0.3)
        bus.take()
        t0 = time.time()
        time.sleep(2.0)
        n = len(split_frames(bus.take()))
        rate = n / (time.time() - t0)
        check(20 <= rate <= 26.5, f"paced at ~25 fps ({rate:.1f})")
        time.sleep(0.2)
        s = t.stats()
        check(2000 <= s["auto_fps_x100"] <= 2650, f"auto_fps_x100 ~ 2500 ({s['auto_fps_x100']})")
        ovl1 = [rect_rec(0, 0, 32, 32, 0, 255, 0)]
        spin = [scene_obj(1, D_ZTEST | D_ZWRITE | D_CULL | D_LIGHT, m1, (0.3, 1, 0.2), 2.0),
                scene_obj(2, D_ZTEST | D_ZWRITE | D_LIGHT, m2, (1, 0, 0), -1.5)]
        check(t.status(T_SCENE, scene_payload(view32, proj32, spin, ovl1, orbit=0.5)) == OK,
              "T_SCENE (spinning objects, orbiting camera, 1 overlay) while running")
        check(t.status(T_AUTO, struct.pack("<II", 1, 0)) == OK, "T_AUTO max_fps=0 while running")
        time.sleep(0.2)
        bus.take()
        time.sleep(0.5)
        frames = split_frames(bus.take())
        fn = [f[0] for f in frames]
        check(len(frames) >= 10 and all(b == a + 1 for a, b in zip(fn, fn[1:])),
              f"frames continue, frame_no consecutive ({len(frames)} frames, {fn[0] if fn else '-'}..)")
        check(all(f[-2 * REC:-REC] == ovl1[0] and rec_type(f, len(f) // REC - 3) == TRI for _, f in frames),
              "new overlay right before END, after the TRIs")
        tri_sets = [f[:-2 * REC] for _, f in frames]
        changed = sum(1 for a, b in zip(tri_sets, tri_sets[1:]) if a != b)
        check(changed >= len(tri_sets) - 2, f"animation: consecutive frames differ ({changed}/{len(tri_sets) - 1})")
        check(fn[0] > fnos[-1], "frame_no kept counting across the scene change")

        print("[autonomous: stalled FPGA (BUSY) -> USB still served inside the frame, timeout, recovery]")
        big = [scene_obj(7, D_ZTEST | D_ZWRITE, as_f32(model_matrix(0, -1.0, 0, 0.3, 0.0, 6.0)), (0, 1, 0), 0.4)]
        check(t.status(T_SCENE, scene_payload(view32, proj32, big, ovl1)) == OK, "T_SCENE with the 7200-tri grid")
        time.sleep(0.2)
        s = t.stats()
        to0, rs_last = s["bus_timeouts"], s["records_sent"]
        t_pause = t_last = time.time()
        bus.paused = True                                  # buffers fill: BUSY = 1 in mid-frame
        # the kernel may still trickle a few records out of its buffers; the 500 ms rule counts
        # from the last record that got through: sample until the timeout shows up
        samples, t_to = [], None
        tend = time.time() + 4.0 * SLOW
        while time.time() < tend:
            t0 = time.time()
            s = t.stats()
            now = time.time()
            samples.append((now - t_pause, now - t0, s["auto_frames"], s["bus_timeouts"]))
            if s["bus_timeouts"] > to0:
                t_to = now
                break
            if s["records_sent"] != rs_last:
                rs_last, t_last = s["records_sent"], now
            time.sleep(0.02)
        stuck = [x for x in samples if x[0] >= t_last - t_pause + 0.03 and x[3] == to0]
        lat = max(x[1] for x in samples)
        check(len(stuck) >= 5 and lat < 0.1 * SLOW,
              f"T_STATS answered while BUSY holds the frame ({len(stuck)} replies, max {lat * 1000:.1f} ms)")
        check(len(set(x[2] for x in stuck)) == 1 and s["auto_running"] == 1,
              "... from inside one frame (auto_frames constant)", str(sorted(set(x[2] for x in stuck))))
        gap = (t_to - t_last) if t_to else -1
        check(t_to is not None and 0.46 <= gap <= 0.6 * SLOW + 0.1,
              f"BUSY stuck: timeout {gap * 1000:.0f} ms after the last record got through")
        h = t.hello()
        check(h["bus_enabled"] == 0 and h["fpga_ready"] == 0 and s["records_sent"] == rs_last,
              "... pins released (high-Z), nothing more sent", str(h))
        fa = s["auto_frames"]
        bus.paused = False
        check(t.wait_ready(1, 3.0), "bus re-armed after BUSY drops")
        time.sleep(0.5)
        check(t.stats()["auto_frames"] > fa, "autonomous frames resume")
        check(t.status(T_SCENE, scene_payload(view32, proj32, spin, ovl1, orbit=0.5)) == OK, "back to the small scene")

        print("[bus disappears while running]")
        bus.stop()
        time.sleep(1.0)
        h = t.hello()
        s = t.stats()
        check(h["fpga_ready"] == 0 and h["bus_enabled"] == 0 and s["auto_running"] == 1,
              "bus gone: not ready, pins high-Z, loop idles", str(h))
        fa = s["auto_frames"]
        time.sleep(0.3)
        check(t.stats()["auto_frames"] == fa, "no frames without a bus")
        bus = FakeBus(bus_port).start()
        check(t.wait_ready(1, 3.0), "bus back (reconnected)")
        time.sleep(0.4)
        frames = split_frames(bus.take())
        check(len(frames) >= 3 and frames[0][0] > fn[-1], "frames flow again, frame_no still increasing",
              f"{len(frames)}")

        print("[T_AUTO stop / T_RESET]")
        check(t.status(T_AUTO, struct.pack("<II", 0, 0)) == OK, "T_AUTO enable=0")
        time.sleep(0.2)
        bus.take()
        time.sleep(0.3)
        check(bus.size() == 0 and t.stats()["auto_running"] == 0, "stopped: bus silent, auto_running = 0")
        check(t.status(T_AUTO, struct.pack("<II", 2, 0)) == E_ARG, "T_AUTO enable=2 -> GPU_ERR_ARG")
        d, _, _ = t.frame(frame_payload(50, vp, draws[:1]))
        check(d["status"] == 0, "T_FRAME works again after stop")
        check(t.status(T_AUTO, struct.pack("<II", 1, 0)) == OK, "restart")
        time.sleep(0.2)
        check(t.status(T_RESET) == OK, "T_RESET")
        time.sleep(0.1)
        bus.take()
        time.sleep(0.2)
        s = t.stats()
        check(bus.size() == 0 and s["auto_running"] == 0, "T_RESET stops autonomous mode")
        d, _, _ = t.frame(frame_payload(51, vp, draws[:1]))
        check(d["status"] == E_ARG, "T_RESET cleared the meshes (T_FRAME -> GPU_ERR_ARG)")
        check(t.status(T_AUTO, struct.pack("<II", 1, 0)) == E_ARG, "T_RESET cleared the scene")

        print("[reconnect of the host link]")
        t.send(T_MESH, meshes[1][:100])                   # half a message, then a new connection
        t.close()
        t = Teensy(port)
        check(t.hello()["status"] == 0, "new connection: partial message dropped, HELLO works")
        s = t.stats()
        check(s["records_sent"] * 24 == s["words_sent"] and s["frames"] >= 5, "final stats", str(s))
        t.close()
        bus.stop()
    finally:
        ref.close()
        stop_proc(sim, "teensy_sim")
        log.close()


def test_daemon():
    print("[end-to-end through the daemon simulator]")
    dport, bport, tport = free_port(), free_port(), free_port()
    dlog = open(os.path.join(ARGS.logdir, "fpgagpud_sim.log"), "w")
    dm = subprocess.Popen([ARGS.daemon, "--sim", "--port", str(dport), "--bus-port", str(bport),
                           "--bind", "127.0.0.1"], stdout=dlog, stderr=subprocess.STDOUT)
    sim, log = start_sim(tport, bport, "teensy_sim_e2e.log")
    ref = Ref(ARGS.ref)
    try:
        n = Link(dport, NMAGIC)
        r = n.call(1)                                               # NET_HELLO -> controller
        check(struct.unpack_from("<i", r)[0] == 0, "daemon NET_HELLO")
        r = n.call(2, struct.pack("<II", 2 | 8, 0x0010))            # SRC_TEENSY | SCANOUT_EN
        check(struct.unpack("<i", r)[0] == 0, "NET_SET_CONFIG SRC_TEENSY|SCANOUT_EN")

        def regs():
            v = struct.unpack("<i24I3I", n.call(10))
            return v[1:25]
        t = Teensy(tport)
        check(t.wait_ready(1, 5.0), "teensy_sim connected to the daemon's bus, armed")
        cube_v, cube_i = cube_mesh()
        sph_v, sph_i = sphere_mesh()
        for mid, (v, i) in ((1, (cube_v, cube_i)), (2, (sph_v, sph_i))):
            pl = mesh_payload(mid, v, i)
            check(t.status(T_MESH, pl) == 0 and ref.mesh(pl) == 0, f"mesh {mid}")
        proj = perspective(math.radians(55), 16 / 9, 0.1, 50.0)
        view = look_at((0.5, 1.2, 4.0), (0, 0, 0), (0, 1, 0))
        draws = [draw(1, D_ZTEST | D_ZWRITE | D_CULL | D_LIGHT, model_matrix(-1.0, 0, 0, 0.7, 0.4)),
                 draw(2, D_ZTEST | D_ZWRITE | D_LIGHT, model_matrix(1.1, 0, 0.3, 0, 0, 0.9))]
        fc0 = regs()[4]
        pl = frame_payload(777, mat_mul(proj, view), draws, F_RETURN)
        d, recs, _ = t.frame(pl)
        check(d["status"] == 0 and d["nrecs"] > 20, f"T_FRAME to the daemon ({d['nrecs']} records returned)")
        r = n.call(9, struct.pack("<II", fc0 + 1, 3000))           # NET_WAIT_FRAME
        check(struct.unpack("<iI", r)[0] == 0, "NET_WAIT_FRAME: the frame was rendered")
        rb = n.call(11)                                             # NET_READBACK
        st, w, h = struct.unpack_from("<iII", rb)
        expect = ref.render(0x0010, recs)
        check(st == 0 and (w, h) == (W, H) and rb[12:] == expect,
              "read-back framebuffer == gpu_refrast(returned records) bit-exact")
        rg = regs()
        check(rg[23] == 777 and rg[12] == 0, "LAST_FRAME_NO = 777, BAD_RECORDS = 0", f"{rg[23]} {rg[12]}")
        objs = [scene_obj(1, D_ZTEST | D_ZWRITE | D_CULL | D_LIGHT, as_f32(model_matrix(-1.0, 0, 0)), (0, 1, 0.3), 1.0),
                scene_obj(2, D_ZTEST | D_ZWRITE | D_LIGHT, as_f32(model_matrix(1.1, 0, 0.3)), (1, 0, 0), 2.0)]
        check(t.status(T_SCENE, scene_payload(as_f32(view), as_f32(proj), objs,
                                              [rect_rec(0, 0, 64, 16, 255, 255, 255)], orbit=0.3)) == 0, "T_SCENE")
        rg0 = regs()
        check(t.status(T_AUTO, struct.pack("<II", 1, 0)) == 0, "T_AUTO start")
        time.sleep(1.5)
        rg1 = regs()
        s = t.stats()
        check(t.status(T_AUTO, struct.pack("<II", 0, 0)) == 0, "T_AUTO stop")
        df = (rg1[4] - rg0[4]) & 0xFFFFFFFF
        check(df >= 10, f"daemon rendered {df} autonomous frames in 1.5 s (Teensy {s['auto_fps_x100'] / 100:.1f} fps)")
        check(rg1[12] == 0 and rg1[11] == 0, "no bad records, no list overflow", f"bad={rg1[12]} ovf={rg1[11]}")
        check(rg1[23] != rg0[23] and 1 <= rg1[23] <= s["auto_frames"],
              f"LAST_FRAME_NO follows the Teensy's autonomous frame counter ({rg1[23]} <= {s['auto_frames']})")
        t.close()
        n.close()
    finally:
        ref.close()
        stop_proc(sim, "teensy_sim (e2e)")
        dm.terminate()
        try:
            dm.wait(timeout=10)
        except subprocess.TimeoutExpired:
            dm.kill()
        log.close()
        dlog.close()


def main():
    global ARGS, SLOW
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default="build/teensy_sim")
    ap.add_argument("--ref", default="build/geom_ref")
    ap.add_argument("--daemon", default="")
    ap.add_argument("--logdir", default="build")
    ap.add_argument("--slow", action="store_true", help="sanitizer builds: longer timeouts")
    ARGS = ap.parse_args()
    SLOW = 3.0 if ARGS.slow else 1.0
    os.makedirs(ARGS.logdir, exist_ok=True)
    t0 = time.time()
    test_sim()
    if ARGS.daemon and os.path.isfile(ARGS.daemon) and os.access(ARGS.daemon, os.X_OK):
        test_daemon()
    else:
        print(f"[end-to-end through the daemon simulator: SKIPPED ({ARGS.daemon or 'no --daemon'} not found)]")
    print(f"test_teensy_sim: {RESULTS['pass']} passed, {RESULTS['fail']} failed ({time.time() - t0:.1f} s)")
    return 1 if RESULTS["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
