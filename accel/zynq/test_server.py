#!/usr/bin/env python3
"""End-to-end tests for zaccel-server (accel/SPEC.md section 4). Python 3 standard library only.

    python3 test_server.py --bin out/zaccel-server-x86 --cpu
    python3 test_server.py --bin out/zaccel-server-x86 --model
    python3 test_server.py --bin out/zaccel-server-armhf --cpu --wrap qemu-arm-static

Every GEMV answer is checked against the pure-Python reference below, which unpacks the weights
itself from SPEC section 1 and shares no code with the server.
"""
import argparse
import itertools
import operator
import os
import random
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback

REQ_MAGIC = 0x3151415A
REP_MAGIC = 0x3152415A
INFO, LOAD, FREE, GEMV, PING = 1, 2, 3, 4, 5
OK, BAD, NOMEM, ENGINE, UNKNOWN = 0, 1, 2, 3, 4
INFO_FIELDS = ("version", "engine", "mem_total_mb", "mem_free_mb", "max_cols", "max_batch", "selftest")

# ---- reference (SPEC section 1) -----------------------------------------------------------
LO = [((b & 15) ^ 8) - 8 for b in range(256)]  # low nibble = weight 2i, signed
HI = [((b >> 4) ^ 8) - 8 for b in range(256)]  # high nibble = weight 2i+1, signed
PAIRS = [(LO[b], HI[b]) for b in range(256)]


def packed_len(mode, cols):
    return (cols + 1) // 2 if mode == 0 else cols


def beats_per_row(mode, cols):
    return (cols + 15) // 16 if mode == 0 else (cols + 7) // 8


def row_bytes(mode, cols):
    """LOAD row length on the wire (SPEC section 4): packed and padded to whole 8-byte beats."""
    return 8 * beats_per_row(mode, cols)


def unpack_row(mode, cols, seg):
    if mode == 0:
        return list(itertools.chain.from_iterable(map(PAIRS.__getitem__, seg)))[:cols]
    return memoryview(bytes(seg[:cols])).cast("b").tolist()


def unpack_rows(mode, rows, cols, wire):
    rb = row_bytes(mode, cols)
    return [unpack_row(mode, cols, wire[r * rb:(r + 1) * rb]) for r in range(rows)]


def unpack_acts(abytes, nb, cols):
    return [memoryview(abytes[v * cols:(v + 1) * cols]).cast("b").tolist() for v in range(nb)]


def ref_gemv(W, A):
    """Y[r][v] = sum_k W[r][k] * A[v][k], flattened in reply order (row-major, [rows][nb])."""
    mul = operator.mul
    return [sum(map(mul, w, a)) for w in W for a in A]


def random_row(rnd, mode, cols):
    """One LOAD row: random weights packed per SPEC section 1, unused nibble and padding 0."""
    wl = packed_len(mode, cols)
    b = bytearray(rnd.randbytes(wl))
    if mode == 0 and cols & 1:
        b[-1] &= 0x0F
    return bytes(b) + bytes(row_bytes(mode, cols) - wl)


def make_case(rnd, mode, rows, cols, nb):
    wire = b"".join(random_row(rnd, mode, cols) for _ in range(rows))
    abytes = rnd.randbytes(nb * cols)
    return wire, abytes, ref_gemv(unpack_rows(mode, rows, cols, wire), unpack_acts(abytes, nb, cols))


# ---- client -------------------------------------------------------------------------------
class Conn:
    def __init__(self, port, timeout=600):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.seq = random.randrange(1 << 32)

    def close(self):
        self.s.close()

    def recv_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.s.recv(min(n - len(buf), 1 << 20))
            if not chunk:
                raise EOFError(f"connection closed after {len(buf)} of {n} bytes")
            buf += chunk
        return bytes(buf)

    def read_reply(self):
        magic, status, seq, ln = struct.unpack("<IIII", self.recv_exact(16))
        if magic != REP_MAGIC:
            raise AssertionError(f"reply magic 0x{magic:08x}")
        return status, seq, self.recv_exact(ln)

    def expect_eof(self, within=5.0):
        self.s.settimeout(within)
        try:
            data = self.s.recv(1)
        except ConnectionResetError:
            data = b""
        if data:
            raise AssertionError("expected the server to close the connection")

    def call(self, op, *parts, length=None, magic=REQ_MAGIC):
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        seq = self.seq
        ln = sum(len(p) for p in parts) if length is None else length
        self.s.sendall(struct.pack("<IIII", magic, op, seq, ln))
        for p in parts:
            self.s.sendall(p)
        status, rseq, payload = self.read_reply()
        if rseq != seq:
            raise AssertionError(f"reply seq {rseq} != request seq {seq}")
        return status, payload

    def info(self):
        st, p = self.call(INFO)
        assert st == OK and len(p) == 28, (st, len(p))
        return dict(zip(INFO_FIELDS, struct.unpack("<7I", p)))

    def load(self, mode, rows, cols, *wire):
        st, p = self.call(LOAD, struct.pack("<III", mode, rows, cols), *wire)
        if st != OK:
            assert not p
            return st, None
        assert len(p) == 4
        return st, struct.unpack("<I", p)[0]

    def free(self, tid):
        st, p = self.call(FREE, struct.pack("<I", tid))
        assert not p
        return st

    def gemv(self, tid, nb, abytes):
        st, p = self.call(GEMV, struct.pack("<II", tid, nb), abytes)
        if st != OK:
            assert not p
            return st, None, None, None
        cycles, used = struct.unpack("<II", p[:8])
        return st, cycles, used, memoryview(p[8:]).cast("i").tolist()


# ---- helpers ------------------------------------------------------------------------------
class Ctx:
    pass


def check_y(Y, expect, what):
    if Y != expect:
        if len(Y) != len(expect):
            raise AssertionError(f"{what}: {len(Y)} results, expected {len(expect)}")
        i = next(i for i in range(len(Y)) if Y[i] != expect[i])
        raise AssertionError(f"{what}: result {i} is {Y[i]}, expected {expect[i]}")


def check_gemv(ctx, st, cycles, used, Y, mode, rows, cols, nb, expect, what):
    assert st == OK, f"{what}: GEMV status {st}"
    assert used == ctx.engine, f"{what}: engine_used {used}, expected {ctx.engine}"
    if ctx.engine == 0:
        assert cycles == 0, f"{what}: cpu cycles {cycles}"
    else:  # model: one beat per clock from the first activation beat to the last weight beat
        want = nb * ((cols + 7) // 8) + rows * beats_per_row(mode, cols)
        assert cycles == want, f"{what}: cycles {cycles}, model predicts {want}"
    check_y(Y, expect, what)


def run_job(ctx, c, mode, rows, cols, nb, rnd, what=None):
    what = what or f"mode {mode} rows {rows} cols {cols} nb {nb}"
    wire, abytes, expect = make_case(rnd, mode, rows, cols, nb)
    st, tid = c.load(mode, rows, cols, wire)
    assert st == OK, f"{what}: LOAD status {st}"
    check_gemv(ctx, *c.gemv(tid, nb, abytes), mode, rows, cols, nb, expect, what)
    assert c.free(tid) == OK, f"{what}: FREE failed"


def wait_mem_free(ctx, want, within=10.0):
    t0 = time.time()
    while True:
        c = Conn(ctx.port)
        got = c.info()["mem_free_mb"]
        c.close()
        if got == want:
            return
        if time.time() - t0 > within:
            raise AssertionError(f"mem_free_mb {got}, expected {want}")
        time.sleep(0.1)


# ---- tests --------------------------------------------------------------------------------
def t_info(ctx):
    c = Conn(ctx.port)
    i = c.info()
    c.close()
    print(f"      INFO {i}")
    ctx.baseline_free = i["mem_free_mb"]
    assert i["version"] == 1
    assert i["engine"] == ctx.engine, f"engine {i['engine']}, expected {ctx.engine}"
    assert i["mem_total_mb"] > 0 and i["mem_free_mb"] <= i["mem_total_mb"]
    assert i["max_cols"] == 4096 and i["max_batch"] == 8
    assert i["selftest"] == 0, f"selftest code 0x{i['selftest']:x}"


def t_ping(ctx):
    c = Conn(ctx.port)
    for n in (0, 1, 7, 1000, 65535, 65536, 1 << 20, 16 << 20):
        data = ctx.rnd.randbytes(n)
        st, p = c.call(PING, data)
        assert st == OK and p == data, f"PING {n} bytes: status {st}, echo {'ok' if p == data else 'differs'}"
    # above the 16 MB PING bound: status 1 and the server closes (after taking the payload)
    st, p = c.call(PING, bytes((16 << 20) + 1))
    assert st == BAD and not p, f"oversized PING status {st}"
    c.expect_eof()
    c.close()


def t_random_jobs(ctx):
    rnd = ctx.rnd
    c = Conn(ctx.port)
    special = [1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 33, 4095, 4096]
    jobs = []
    for i, cols in enumerate(special):
        for mode in (0, 1):
            nb = (2 * i + mode) % 8 + 1
            rows = rnd.randint(1, 600)
            jobs.append((mode, rows, cols, nb))
    jobs += [(0, 1, 17, 8), (1, 600, 17, 8), (0, 600, 4096, 8), (1, 1, 4096, 1)]
    for _ in range(30):
        jobs.append((rnd.randint(0, 1), rnd.randint(1, 600), rnd.randint(1, 4096), rnd.randint(1, 8)))
    for mode, rows, cols, nb in jobs:
        run_job(ctx, c, mode, rows, cols, nb, rnd)
    print(f"      {len(jobs)} LOAD/GEMV/FREE jobs verified (modes 0+1, nb 1..8, cols incl. 1,17,4095,4096, rows 1..600)")
    c.close()
    wait_mem_free(ctx, ctx.baseline_free)


def t_extremes(ctx):
    c = Conn(ctx.port)
    rows, cols, nb = 3, 4096, 8
    for mode, wbyte, want in ((1, 0x80, 4096 * 128 * 128), (1, 0x7F, -4096 * 127 * 128),
                              (0, 0x88, 4096 * 8 * 128), (0, 0x77, -4096 * 7 * 128)):
        wire = bytes([wbyte]) * (rows * row_bytes(mode, cols))  # 4096 cols: no padding
        abytes = bytes([0x80]) * (nb * cols)  # every activation -128
        st, tid = c.load(mode, rows, cols, wire)
        assert st == OK
        expect = ref_gemv(unpack_rows(mode, rows, cols, wire), unpack_acts(abytes, nb, cols))
        assert expect == [want] * (rows * nb)
        check_gemv(ctx, *c.gemv(tid, nb, abytes), mode, rows, cols, nb, expect, f"extreme mode {mode} w 0x{wbyte:02x}")
        assert c.free(tid) == OK
    c.close()


def t_load_length(ctx):
    """LOAD takes exactly rows x rowbytes (rows padded to whole beats); any other length is status 1."""
    c = Conn(ctx.port)
    for mode, rows, cols, nb in ((0, 5, 17, 3), (1, 4, 9, 2), (1, 3, 1, 1), (0, 2, 4081, 8)):
        wire, abytes, expect = make_case(ctx.rnd, mode, rows, cols, nb)
        wl, rb = packed_len(mode, cols), row_bytes(mode, cols)
        assert wl != rb
        unpadded = b"".join(wire[r * rb:r * rb + wl] for r in range(rows))
        for name, form in (("unpadded rows", unpadded), ("one byte short", wire[:-1]), ("one byte long", wire + b"\0"),
                           ("one extra row", wire + wire[:rb])):
            st, tid = c.load(mode, rows, cols, form)
            assert st == BAD and tid is None, f"{name} mode {mode} cols {cols}: status {st}"
        st, tid = c.load(mode, rows, cols, wire)
        assert st == OK
        check_gemv(ctx, *c.gemv(tid, nb, abytes), mode, rows, cols, nb, expect, f"padded mode {mode} cols {cols}")
        assert c.free(tid) == OK
    c.close()
    wait_mem_free(ctx, ctx.baseline_free)


def t_unknown_tensor(ctx):
    c = Conn(ctx.port)
    assert c.gemv(0xDEADBEEF, 1, b"\x01")[0] == UNKNOWN
    assert c.free(0xDEADBEEF) == UNKNOWN
    st, tid = c.load(1, 2, 3, b"\x01\x02\x03" + bytes(5) + b"\x04\x05\x06" + bytes(5))
    assert st == OK
    assert c.free(tid) == OK
    assert c.gemv(tid, 1, b"\x01\x02\x03")[0] == UNKNOWN, "GEMV on a freed tensor"
    assert c.free(tid) == UNKNOWN, "double FREE"
    c.close()


def t_bad_requests(ctx):
    """Well-framed bad requests get status 1 and the connection keeps working."""
    c = Conn(ctx.port)
    mode, rows, cols = 1, 2, 5
    wire, abytes, expect = make_case(ctx.rnd, mode, rows, cols, 2)
    st, tid = c.load(mode, rows, cols, wire)
    assert st == OK
    p = struct.pack
    bad = [
        ("GEMV nb 0", GEMV, [p("<II", tid, 0)]),
        ("GEMV nb 9", GEMV, [p("<II", tid, 9), bytes(9 * cols)]),
        ("GEMV payload 1 long", GEMV, [p("<II", tid, 2), bytes(2 * cols + 1)]),
        ("GEMV payload 1 short", GEMV, [p("<II", tid, 2), bytes(2 * cols - 1)]),
        ("GEMV len 4", GEMV, [p("<I", tid)]),
        ("LOAD mode 2", LOAD, [p("<III", 2, 1, 8), bytes(8)]),
        ("LOAD rows 0", LOAD, [p("<III", 1, 0, 8)]),
        ("LOAD rows 65536", LOAD, [p("<III", 1, 65536, 1), bytes(65536)]),
        ("LOAD cols 0", LOAD, [p("<III", 1, 1, 0)]),
        ("LOAD cols 4097", LOAD, [p("<III", 1, 1, 4097), bytes(4097)]),
        ("LOAD payload 1 short", LOAD, [p("<III", 1, 3, 10), bytes(3 * row_bytes(1, 10) - 1)]),
        ("LOAD payload 1 long", LOAD, [p("<III", 0, 3, 10), bytes(3 * row_bytes(0, 10) + 1)]),
        ("LOAD len 8", LOAD, [p("<II", 1, 1)]),
        ("INFO with payload", INFO, [bytes(4)]),
        ("FREE len 3", FREE, [bytes(3)]),
        ("FREE len 8", FREE, [p("<II", tid, 0)]),
        ("op 0", 0, [bytes(10)]),
        ("op 6", 6, []),
        ("op 99", 99, [bytes(1000)]),
        ("op 0xFFFFFFFF", 0xFFFFFFFF, [bytes(3)]),
    ]
    for name, op, parts in bad:
        st, pl = c.call(op, *parts)
        assert st == BAD and not pl, f"{name}: status {st}"
        st, pl = c.call(PING, b"alive")
        assert st == OK and pl == b"alive", f"connection unusable after {name}"
    check_gemv(ctx, *c.gemv(tid, 2, abytes), mode, rows, cols, 2, expect, "tensor after bad requests")
    assert c.free(tid) == OK
    c.close()
    wait_mem_free(ctx, ctx.baseline_free)


def t_bad_magic_and_garbage(ctx):
    """Bad magic: status 1 (seq echoed) and the server closes; it keeps serving others."""
    c = Conn(ctx.port)
    c.s.sendall(struct.pack("<IIII", 0x12345678, INFO, 77, 0))
    st, seq, pl = c.read_reply()
    assert st == BAD and seq == 77 and not pl, (st, seq)
    c.expect_eof()
    c.close()

    for n in (16, 300, 100000):
        c = Conn(ctx.port)
        g = bytearray(ctx.rnd.randbytes(n))
        if struct.unpack("<I", g[:4])[0] == REQ_MAGIC:
            g[0] ^= 0xFF
        c.s.sendall(g)
        st, seq, pl = c.read_reply()
        assert st == BAD and seq == struct.unpack("<I", g[8:12])[0], (st, seq)
        c.expect_eof()
        c.close()

    # a header announcing 2 GB of payload for an unknown op: answer and close, do not wait for it
    c = Conn(ctx.port)
    st, pl = c.call(99, length=0x7FFFFFFF)
    assert st == BAD
    c.expect_eof()
    c.close()

    c = Conn(ctx.port)
    assert c.info()["version"] == 1
    c.close()


def t_disconnects(ctx):
    base = ctx.baseline_free
    # LOAD cut off mid-payload: the half-filled tensor must be released
    c = Conn(ctx.port)
    c.s.sendall(struct.pack("<IIII", REQ_MAGIC, LOAD, 1, 12 + 4000 * 4096) + struct.pack("<III", 1, 4000, 4096) + bytes(100000))
    c.close()
    # header cut short
    c = Conn(ctx.port)
    c.s.sendall(struct.pack("<II", REQ_MAGIC, INFO))
    c.close()
    # GEMV sent, reply never read
    c = Conn(ctx.port)
    wire, abytes, _ = make_case(ctx.rnd, 1, 600, 4096, 8)
    st, tid = c.load(1, 600, 4096, wire)
    assert st == OK
    c.s.sendall(struct.pack("<IIII", REQ_MAGIC, GEMV, 5, 8 + len(abytes)) + struct.pack("<II", tid, 8) + abytes)
    c.close()
    # many connect/close
    for _ in range(20):
        Conn(ctx.port).close()
    c = Conn(ctx.port)
    assert c.free(tid) == OK, "tensor from a closed connection"
    c.close()
    wait_mem_free(ctx, base)


def t_concurrent(ctx):
    """4 clients at once: private tensors plus one shared tensor used from every connection."""
    main = Conn(ctx.port)
    smode, srows, scols = 0, 300, 1000
    swire, _, _ = make_case(ctx.rnd, smode, srows, scols, 1)
    sW = unpack_rows(smode, srows, scols, swire)
    st, shared = main.load(smode, srows, scols, swire)
    assert st == OK
    errors = []
    counts = [0] * 4

    def worker(k):
        try:
            rnd = random.Random(ctx.seed * 100 + k)
            c = Conn(ctx.port)
            for j in range(8):
                if j % 2:
                    nb = rnd.randint(1, 8)
                    abytes = rnd.randbytes(nb * scols)
                    expect = ref_gemv(sW, unpack_acts(abytes, nb, scols))
                    check_gemv(ctx, *c.gemv(shared, nb, abytes), smode, srows, scols, nb, expect, f"client {k} shared")
                else:
                    run_job(ctx, c, rnd.randint(0, 1), rnd.randint(1, 300), rnd.randint(1, 1024), rnd.randint(1, 8), rnd,
                            f"client {k} job {j}")
                st2, p2 = c.call(PING, b"x" * j)
                assert st2 == OK and p2 == b"x" * j
                counts[k] += 1
            c.close()
        except Exception as e:  # noqa: BLE001 - reported below
            errors.append(f"client {k}: {e!r}")

    th = [threading.Thread(target=worker, args=(k,)) for k in range(4)]
    for t in th:
        t.start()
    for t in th:
        t.join()
    assert not errors, "; ".join(errors)
    assert counts == [8] * 4, counts
    assert main.free(shared) == OK
    main.close()
    wait_mem_free(ctx, ctx.baseline_free)


def t_large(ctx):
    """rows 4096 x cols 4096, int4, nb 4."""
    c = Conn(ctx.port)
    run_job(ctx, c, 0, 4096, 4096, 4, ctx.rnd, "large 4096x4096 int4 nb 4")
    c.close()


def t_over_26bit(ctx):
    """int8 16400 x 4096 = 67 174 400 bytes of weights, past the DMA's 26-bit length register."""
    mode, rows, cols, nb, period = 1, 16400, 4096, 2, 7
    pattern = [ctx.rnd.randbytes(cols) for _ in range(period)]
    wire = b"".join(pattern[r % period] for r in range(rows))
    abytes = ctx.rnd.randbytes(nb * cols)
    ypat = ref_gemv([unpack_row(mode, cols, p) for p in pattern], unpack_acts(abytes, nb, cols))
    expect = [ypat[(r % period) * nb + v] for r in range(rows) for v in range(nb)]
    c = Conn(ctx.port)
    st, tid = c.load(mode, rows, cols, wire)
    assert st == OK
    check_gemv(ctx, *c.gemv(tid, nb, abytes), mode, rows, cols, nb, expect, "16400x4096 int8")
    assert c.free(tid) == OK
    c.close()


def t_no_memory(ctx):
    """Tensors of ~60% of the free memory until it is full: status 2, then everything is given back.
    (Sized from INFO: the board's engine window is 224 MB, smaller than the protocol's largest tensor.)"""
    cols = 4096
    c = Conn(ctx.port)
    free = c.info()["mem_free_mb"] << 20
    c.close()
    mode, rows = 1, max(1, min(65535, int(free * 0.6) // cols))
    wire = b"\x01" * (rows * cols)  # every weight 1, so Y[r] = sum(A)
    c = Conn(ctx.port)
    ids, statuses = [], []
    for _ in range(3):
        st, tid = c.load(mode, rows, cols, wire)
        statuses.append(st)
        if st != OK:
            break
        ids.append(tid)
    assert statuses[-1] == NOMEM and ids, f"LOAD statuses {statuses}"
    abytes = ctx.rnd.randbytes(cols)
    s = sum(memoryview(abytes).cast("b").tolist())
    check_gemv(ctx, *c.gemv(ids[0], 1, abytes), mode, rows, cols, 1, [s] * rows, f"{rows}x4096 int8")
    assert c.call(PING, b"after nomem")[0] == OK
    for tid in ids:
        assert c.free(tid) == OK
    c.close()
    wait_mem_free(ctx, ctx.baseline_free)
    print(f"      {len(ids)} x {rows * cols >> 20} MB tensor loaded (of {free >> 20} MB free), next LOAD -> status 2")


def t_model_faults(ctx):
    """--model only: an engine error answers status 3 and the server carries on; a PL that fails
    its selftest is replaced by the cpu engine."""
    proc, port, log = start_server(ctx.args, ["--model", "--model-fault", "2"], "fault2")
    try:
        c = Conn(port)
        i = c.info()
        assert i["engine"] == 1 and i["selftest"] == 0, i
        rnd = ctx.rnd
        for j in (1, 2, 3):
            wire, abytes, expect = make_case(rnd, 0, 40, 100, 3)
            st, tid = c.load(0, 40, 100, wire)
            assert st == OK
            st, cycles, used, Y = c.gemv(tid, 3, abytes)
            if j == 2:
                assert st == ENGINE, f"faulted job: status {st}"
            else:
                check_gemv(ctx, st, cycles, used, Y, 0, 40, 100, 3, expect, f"job {j} around the fault")
            assert c.free(tid) == OK
        assert c.info()["engine"] == 1
        c.close()
        assert proc.poll() is None, "server died"
    finally:
        stop_server(proc)
    proc, port, log = start_server(ctx.args, ["--model", "--model-fault", "-1"], "faultst")
    try:
        c = Conn(port)
        i = c.info()
        assert i["engine"] == 0 and i["selftest"] != 0, i
        wire, abytes, expect = make_case(ctx.rnd, 1, 20, 50, 2)
        st, tid = c.load(1, 20, 50, wire)
        assert st == OK
        st, cycles, used, Y = c.gemv(tid, 2, abytes)
        assert st == OK and used == 0 and cycles == 0
        check_y(Y, expect, "cpu fallback after failed selftest")
        c.close()
        print(f"      selftest fault -> engine 0, selftest code 0x{i['selftest']:x}")
    finally:
        stop_server(proc)


# ---- server process -----------------------------------------------------------------------
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_server(args, extra, tag):
    port = free_port()
    cmd = ([args.wrap] if args.wrap else []) + [args.bin, "--port", str(port)] + extra
    os.makedirs(args.logdir, exist_ok=True)
    log = os.path.join(args.logdir, f"server-{os.path.basename(args.bin)}-{tag}.log")
    proc = subprocess.Popen(cmd, stdout=open(log, "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    while True:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited with {proc.returncode}; log:\n{open(log).read()}")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return proc, port, log
        except OSError:
            if time.time() - t0 > 120:
                proc.kill()
                raise RuntimeError("server did not start listening")
            time.sleep(0.05)


def stop_server(proc):
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--cpu", action="store_true")
    g.add_argument("--model", action="store_true")
    ap.add_argument("--wrap", default="", help="run the server under this (e.g. qemu-arm-static)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--logdir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"))
    args = ap.parse_args()

    ctx = Ctx()
    ctx.args = args
    ctx.seed = args.seed if args.seed is not None else int(time.time()) % 1000000
    ctx.rnd = random.Random(ctx.seed)
    ctx.engine = 1 if args.model else 0
    mode = "model" if args.model else "cpu"
    label = f"{os.path.basename(args.bin)} --{mode}" + (f" under {args.wrap}" if args.wrap else "")
    print(f"=== {label}  (seed {ctx.seed})")

    # the cpu engine runs with a 256 MB arena here, so the 67 MB t_over_26bit tensor fits; the board's
    # default (64 MB, for a 512 MB board) is checked separately below
    proc, ctx.port, log = start_server(args, ["--cpu", "--cpu-mb", "256"] if mode == "cpu" else ["--" + mode], mode)
    tests = [t_info, t_ping, t_random_jobs, t_extremes, t_load_length, t_unknown_tensor, t_bad_requests,
             t_bad_magic_and_garbage, t_disconnects, t_concurrent, t_large, t_over_26bit, t_no_memory]
    failed = []
    try:
        for t in tests:
            try:
                t(ctx)
                print(f"PASS  {t.__name__}")
            except Exception as e:  # noqa: BLE001 - every failure is reported
                failed.append(t.__name__)
                print(f"FAIL  {t.__name__}: {e!r}")
                traceback.print_exc(limit=3)
            if proc.poll() is not None:
                failed.append("server-alive")
                print(f"FAIL  server exited with {proc.returncode} after {t.__name__}")
                break
        if proc.poll() is None:
            print("PASS  server still up after all tests")
    finally:
        stop_server(proc)
    if not args.model:
        p2, port2, _ = start_server(args, ["--cpu"], "cpu-default")
        try:
            i = Conn(port2).info()
            if i["mem_total_mb"] == 64:
                print("PASS  cpu engine default arena is 64 MB (fits the board's 256 MB of Linux RAM)")
            else:
                failed.append("cpu-default")
                print(f"FAIL  cpu engine default arena is {i['mem_total_mb']} MB, want 64")
        finally:
            stop_server(p2)
    if args.model and "server-alive" not in failed:
        try:
            t_model_faults(ctx)
            print("PASS  t_model_faults")
        except Exception as e:  # noqa: BLE001
            failed.append("t_model_faults")
            print(f"FAIL  t_model_faults: {e!r}")
            traceback.print_exc(limit=3)
    print("--- server log (tail) ---")
    print("".join(open(log).readlines()[-8:]), end="")
    if failed:
        print(f"=== {label}: FAILED {failed}")
        return 1
    print(f"=== {label}: ALL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
