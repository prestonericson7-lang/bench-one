#!/usr/bin/env python3
"""test_py.py HOST PORT -- tests zaccel.py against a server (mock_server.py in the tests).
Exit 0 only if every check passed."""
import os
import random
import socket
import struct
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zaccel  # noqa: E402

PASSED = FAILED = 0
rng = random.Random(20260925)
EDGE = [1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255, 257, 1000, 4095, 4096]


def check(cond, msg):
    global PASSED, FAILED
    if cond:
        PASSED += 1
    else:
        FAILED += 1
        print('FAIL', msg)


def expect_status(fn, status, msg):
    try:
        fn()
        check(False, '%s: no error raised' % msg)
    except zaccel.ZaccelError as e:
        check(e.status == status, '%s: status %d, expected %d' % (msg, e.status, status))


def expect_raise(fn, exc, msg):
    try:
        fn()
        check(False, '%s: no %s raised' % (msg, exc.__name__))
    except exc:
        check(True, msg)


def rand_w(mode, n):
    return [rng.randint(-8, 7) if mode == 0 else rng.randint(-128, 127) for _ in range(n)]


def rand_a(n):
    return [rng.randint(-128, 127) for _ in range(n)]


def naive(w, rows, cols, a, nb):
    return [[sum(w[r * cols + k] * a[v * cols + k] for k in range(cols)) for v in range(nb)]
            for r in range(rows)]


def pack(mode, w, rows, cols):
    return zaccel.pack_int4(w, rows, cols) if mode == 0 else zaccel.pack_int8(w, rows, cols)


def offline():
    check(zaccel.row_bytes(0, 1) == 8 and zaccel.row_bytes(0, 17) == 16 and
          zaccel.row_bytes(0, 4096) == 2048 and zaccel.row_bytes(1, 9) == 16 and
          zaccel.row_bytes(1, 4096) == 4096, 'row_bytes')
    expect_raise(lambda: zaccel.row_bytes(2, 8), ValueError, 'row_bytes bad mode')
    for _ in range(40):
        rows, cols = rng.randint(1, 6), rng.choice(EDGE[:17] + [rng.randint(1, 300)])
        w = rand_w(0, rows * cols)
        p = zaccel.pack_int4(w, rows, cols)
        rb = zaccel.row_bytes(0, cols)
        ok = len(p) == rows * rb
        for r in range(rows):
            for k in range(2 * rb):
                nib = (p[r * rb + k // 2] >> (4 * (k % 2))) & 15
                ok &= nib == ((w[r * cols + k] & 15) if k < cols else 0)
        check(ok, 'pack_int4 layout %dx%d' % (rows, cols))
        nested = [w[r * cols:(r + 1) * cols] for r in range(rows)]
        check(zaccel.pack_int4(nested, rows, cols) == p, 'pack_int4 nested == flat')
        w8 = rand_w(1, rows * cols)
        p8 = zaccel.pack_int8(w8, rows, cols)
        rb8 = zaccel.row_bytes(1, cols)
        check(all(p8[r * rb8 + k] == ((w8[r * cols + k] & 0xFF) if k < cols else 0)
                  for r in range(rows) for k in range(rb8)), 'pack_int8 layout %dx%d' % (rows, cols))
    expect_raise(lambda: zaccel.pack_int4([8], 1, 1), ValueError, 'pack_int4 rejects 8')
    expect_raise(lambda: zaccel.pack_int4([-9], 1, 1), ValueError, 'pack_int4 rejects -9')
    expect_raise(lambda: zaccel.pack_int8([128], 1, 1), ValueError, 'pack_int8 rejects 128')
    expect_raise(lambda: zaccel.pack_int8([1, 2], 1, 3), ValueError, 'pack_int8 length check')
    for _ in range(30):
        mode, rows, cols, nb = rng.randint(0, 1), rng.randint(1, 5), rng.choice(EDGE), rng.randint(1, 8)
        w, a = rand_w(mode, rows * cols), rand_a(nb * cols)
        check(zaccel.ref_gemv(mode, rows, cols, pack(mode, w, rows, cols), a, nb) ==
              naive(w, rows, cols, a, nb), 'ref_gemv vs naive mode %d %dx%d nb %d' % (mode, rows, cols, nb))


def online(host, port):
    with zaccel.connect(host, port) as z:
        i = z.info()
        check(i['version'] == 1 and i['max_cols'] == 4096 and i['max_batch'] == 8 and
              i['selftest'] == 0 and i['engine'] in (0, 1) and i['mem_free_mb'] <= i['mem_total_mb'],
              'INFO %r' % i)
        for n in (0, 1, 7, 1000, 65536):
            data = bytes(rng.getrandbits(8) for _ in range(n))
            check(z.ping(data) == data, 'PING %d bytes' % n)

        forms = ['bytes', 'flat', 'nested', 'flat+nb']
        bad = total = 0
        for mode in (0, 1):
            for nb in range(1, 9):
                for it in range(2):
                    cols = rng.choice(EDGE) if it == 0 else rng.randint(1, 700) | 1
                    rows = rng.randint(1, 6 if cols > 2000 else 30)
                    w, a = rand_w(mode, rows * cols), rand_a(nb * cols)
                    t = z.load(mode, rows, cols, pack(mode, w, rows, cols))
                    form = forms[(nb + it) % 4]
                    if form == 'bytes':
                        y, cyc, eng = z.gemv(t, struct.pack('%db' % len(a), *a))
                    elif form == 'flat':
                        y, cyc, eng = z.gemv(t, a)
                    elif form == 'nested':
                        y, cyc, eng = z.gemv(t, [a[v * cols:(v + 1) * cols] for v in range(nb)])
                    else:
                        y, cyc, eng = z.gemv(t, a, nb)
                    want = naive(w, rows, cols, a, nb)
                    total += 1
                    if y != want or eng not in (0, 1):
                        bad += 1
                        print('  wrong: mode %d %dx%d nb %d form %s' % (mode, rows, cols, nb, form))
                    z.free(t)
        check(bad == 0, 'LOAD/GEMV/FREE vs naive: %d of %d wrong' % (bad, total))

        # errors, and the connection survives them
        w, a = rand_w(0, 20 * 64), rand_a(9 * 64)
        t = z.load(0, 20, 64, zaccel.pack_int4(w, 20, 64))
        expect_status(lambda: z.gemv(zaccel.Tensor(t.id, 0, 20, 60), a[:60]), 1, 'GEMV wrong cols')
        expect_status(lambda: z.gemv(t, a, 9), 1, 'GEMV nb=9')
        expect_status(lambda: z.load(0, 1, 4097, bytes(zaccel.row_bytes(0, 4097))), 1, 'LOAD cols 4097')
        check(z.ping(b'ok') == b'ok', 'alive after status 1')
        z.free(t)
        expect_status(lambda: z.gemv(t, a[:64]), 4, 'GEMV on a freed tensor')
        expect_status(lambda: z.free(t), 4, 'FREE twice')
        expect_status(lambda: z.free(zaccel.Tensor(0xDEADBEEF, 0, 1, 1)), 4, 'FREE unknown id')
        check(z.ping(b'ok') == b'ok', 'alive after status 4')
        expect_raise(lambda: z.load(0, 2, 16, b'\0' * 8), ValueError, 'LOAD packed length checked locally')
        expect_raise(lambda: z.gemv(t, b'\0' * 63), ValueError, 'GEMV activation length checked locally')
        expect_raise(lambda: z.gemv(t, [200] * 64), ValueError, 'GEMV activation range checked locally')
    check(z._sock is None, 'context manager closes the socket')

    os.environ['ZACCEL_HOST'] = host
    with zaccel.connect(port=port) as z2:
        check(z2.host == host and z2.ping(b'h') == b'h', 'connect() uses $ZACCEL_HOST')
    del os.environ['ZACCEL_HOST']
    check(zaccel.default_host() == '10.20.0.2', 'default host 10.20.0.2')
    expect_raise(lambda: zaccel.connect('127.0.0.1', 1), ConnectionRefusedError, 'closed port refused')
    check(zaccel._main(['zaccel.py', host, str(port)]) == 0, 'CLI prints INFO')

    if zaccel.np is not None:
        np = zaccel.np
        with zaccel.connect(host, port) as z:
            for mode in (0, 1):
                rows, cols, nb = 17, 333, 5
                w = np.array(rand_w(mode, rows * cols), dtype=np.int8).reshape(rows, cols)
                a = np.array(rand_a(nb * cols), dtype=np.int8).reshape(nb, cols)
                p = pack(mode, w, rows, cols)
                check(p == pack(mode, w.ravel().tolist(), rows, cols), 'numpy pack == list pack')
                t = z.load(mode, rows, cols, p)
                y, _, _ = z.gemv(t, a)
                want = w.astype(np.int64) @ a.astype(np.int64).T
                check(isinstance(y, np.ndarray) and y.shape == (rows, nb) and (y == want).all(),
                      'numpy GEMV mode %d' % mode)
                z.free(t)
    else:
        print('numpy not installed: the numpy paths were NOT tested')


def malformed():
    """A server that answers with a bad magic: ProtocolError, and the connection is closed."""
    ls = socket.socket()
    ls.bind(('127.0.0.1', 0))
    ls.listen(1)

    def evil():
        c, _ = ls.accept()
        h = c.recv(16)
        c.sendall(struct.pack('<4I', 0x12345678, 0, struct.unpack('<4I', h)[2], 0))
        c.close()
    th = threading.Thread(target=evil)
    th.start()
    z = zaccel.connect('127.0.0.1', ls.getsockname()[1])
    try:
        z.info()
        check(False, 'bad magic accepted')
    except zaccel.ProtocolError as e:
        check(e.status == -2 and z._sock is None, 'bad magic -> ProtocolError, connection closed')
    th.join()
    ls.close()


def main():
    if len(sys.argv) != 3:
        print('usage: test_py.py HOST PORT')
        return 2
    offline()
    online(sys.argv[1], int(sys.argv[2]))
    malformed()
    print('test_py: %d passed, %d failed' % (PASSED, FAILED))
    return 1 if FAILED else 0


if __name__ == '__main__':
    sys.exit(main())
