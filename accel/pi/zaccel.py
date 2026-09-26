#!/usr/bin/env python3
"""zaccel.py -- the Orange Pi's Python client for the Zynq matrix engine (accel/SPEC.md §4, §5).

Python 3 standard library only.  numpy is used when it is installed and you hand in numpy arrays.

    import zaccel
    with zaccel.connect() as z:                  # $ZACCEL_HOST or 10.20.0.2, port 8093
        packed = zaccel.pack_int4(w, rows, cols) # w: rows*cols ints in -8..7 (flat, nested, numpy)
        t = z.load(zaccel.MODE_INT4, rows, cols, packed)
        y, cycles, engine = z.gemv(t, a)         # a: nb x cols int8  ->  y: rows x nb int32
        z.free(t)

Packed rows follow SPEC §1: each row starts on a fresh 8-byte beat (row_bytes(mode, cols) bytes,
zero tail).  int4 byte i holds weight 2i in bits 3:0 and weight 2i+1 in bits 7:4.

    python3 zaccel.py [host] [port]     prints the Zynq's INFO and the link's round trip
"""
import array
import itertools
import os
import socket
import struct
import sys
import time

try:
    import numpy as np
except ImportError:          # numpy is optional
    np = None

DEFAULT_PORT = 8093
DEFAULT_HOST = '10.20.0.2'
ALT_HOST = '10.77.0.2'       # the GPU's direct-cable address; tried when no host is given
MAX_COLS = 4096
MAX_BATCH = 8
MODE_INT4 = 0
MODE_INT8 = 1

OK, ST_BADREQ, ST_NOMEM, ST_ENGINE, ST_NOTENSOR = 0, 1, 2, 3, 4
STATUS_TEXT = {ST_BADREQ: 'server: bad request', ST_NOMEM: 'server: no memory',
               ST_ENGINE: 'server: engine error', ST_NOTENSOR: 'server: unknown tensor'}

_REQ_MAGIC = 0x3151415A     # "ZAQ1"
_REP_MAGIC = 0x3152415A     # "ZAR1"
_OP_INFO, _OP_LOAD, _OP_FREE, _OP_GEMV, _OP_PING = 1, 2, 3, 4, 5
_DRAIN_CAP = 1 << 20


class ZaccelError(Exception):
    """A server status (1..4) or, with status -2, a malformed reply (the connection is closed)."""

    def __init__(self, status, msg=None):
        self.status = status
        super().__init__(msg or STATUS_TEXT.get(status, 'status %d' % status))


class ProtocolError(ZaccelError):
    def __init__(self, msg):
        super().__init__(-2, 'malformed reply (connection closed): ' + msg)


# ---------------------------------------------------------------------------------------------
def default_host():
    return os.environ.get('ZACCEL_HOST') or DEFAULT_HOST


def row_bytes(mode, cols):
    if mode == MODE_INT4:
        return 8 * ((cols + 15) // 16)
    if mode == MODE_INT8:
        return 8 * ((cols + 7) // 8)
    raise ValueError('mode must be 0 (int4) or 1 (int8)')


def _is_np(x):
    return np is not None and isinstance(x, np.ndarray)


def _flat(x):
    """Flatten one level of nesting: [[..],[..]] -> [...]."""
    x = list(x)
    if x and not isinstance(x[0], int) and hasattr(x[0], '__len__'):
        return list(itertools.chain.from_iterable(x))
    return x


def _int8_bytes(x):
    """int8 values (bytes, flat or nested ints, numpy) -> raw int8 bytes."""
    if isinstance(x, (bytes, bytearray, memoryview)):
        return bytes(x)
    if _is_np(x):
        if x.dtype != np.int8:
            if x.size and (x.min() < -128 or x.max() > 127):
                raise ValueError('value outside int8')
            x = x.astype(np.int8)
        return np.ascontiguousarray(x).tobytes()
    try:
        return array.array('b', _flat(x)).tobytes()
    except OverflowError:
        raise ValueError('value outside int8') from None


def pack_int4(w, rows, cols):
    """rows x cols weights in -8..7 -> packed rows (bytes)."""
    rb = row_bytes(MODE_INT4, cols)
    if _is_np(w):
        m = np.asarray(w).reshape(rows, cols)
        if m.size and (m.min() < -8 or m.max() > 7):
            raise ValueError('int4 weight outside -8..7')
        pad = np.zeros((rows, 2 * rb), dtype=np.uint8)
        pad[:, :cols] = m.astype(np.int8).view(np.uint8) & 0xF
        return (pad[:, 0::2] | (pad[:, 1::2] << 4)).astype(np.uint8).tobytes()
    v = _flat(w)
    if len(v) != rows * cols:
        raise ValueError('expected %d weights, got %d' % (rows * cols, len(v)))
    if any(x < -8 or x > 7 for x in v):
        raise ValueError('int4 weight outside -8..7')
    out = bytearray(rows * rb)
    for r in range(rows):
        base, o = r * cols, r * rb
        for k in range(0, cols, 2):
            lo = v[base + k] & 0xF
            hi = (v[base + k + 1] & 0xF) if k + 1 < cols else 0
            out[o + (k >> 1)] = lo | (hi << 4)
    return bytes(out)


def pack_int8(w, rows, cols):
    """rows x cols int8 weights -> packed rows (bytes), each padded to 8 bytes."""
    rb = row_bytes(MODE_INT8, cols)
    raw = _int8_bytes(w)
    if len(raw) != rows * cols:
        raise ValueError('expected %d weights, got %d' % (rows * cols, len(raw)))
    out = bytearray(rows * rb)
    for r in range(rows):
        out[r * rb:r * rb + cols] = raw[r * cols:(r + 1) * cols]
    return bytes(out)


def _row_weights(mode, packed, r, cols):
    rb = row_bytes(mode, cols)
    seg = packed[r * rb:(r + 1) * rb]
    if mode == MODE_INT8:
        return struct.unpack('%db' % cols, seg[:cols])
    w = []
    for b in seg[:(cols + 1) // 2]:
        lo, hi = b & 0xF, b >> 4
        w.append(lo - 16 if lo & 8 else lo)
        w.append(hi - 16 if hi & 8 else hi)
    return w[:cols]


def ref_gemv(mode, rows, cols, packed, a, nb=None):
    """The exact reference: y[r][v] = sum_k W[r][k] * A[v][k], as rows lists of nb ints."""
    raw = _int8_bytes(a)
    if nb is None:
        nb = len(raw) // cols
    if len(raw) != nb * cols:
        raise ValueError('activations: expected %d bytes, got %d' % (nb * cols, len(raw)))
    acts = [struct.unpack_from('%db' % cols, raw, v * cols) for v in range(nb)]
    out = []
    for r in range(rows):
        w = _row_weights(mode, packed, r, cols)
        out.append([sum(x * y for x, y in zip(w, av)) for av in acts])
    return out


class Tensor:
    __slots__ = ('id', 'mode', 'rows', 'cols')

    def __init__(self, tid, mode, rows, cols):
        self.id, self.mode, self.rows, self.cols = tid, mode, rows, cols

    def __repr__(self):
        return 'Tensor(id=%d, %s, %dx%d)' % (self.id, 'int4' if self.mode == 0 else 'int8',
                                              self.rows, self.cols)


class Client:
    """One TCP connection to zaccel-server; one request at a time."""

    def __init__(self, host=None, port=None, timeout=60.0, connect_timeout=3.0):
        env = os.environ.get('ZACCEL_TIMEOUT_MS')
        if env:
            timeout = connect_timeout = int(env) / 1000.0
        self.port = port or DEFAULT_PORT
        if host or os.environ.get('ZACCEL_HOST'):
            self.host = host or default_host()
            self._sock = socket.create_connection((self.host, self.port), timeout=connect_timeout)
        else:                    # the Pi may only have a route to one of the Zynq's two addresses
            try:
                self.host = DEFAULT_HOST
                self._sock = socket.create_connection((self.host, self.port), timeout=connect_timeout)
            except OSError:
                self.host = ALT_HOST
                self._sock = socket.create_connection((self.host, self.port), timeout=connect_timeout)
        self._sock.settimeout(timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._seq = 1

    # -- plumbing ---------------------------------------------------------------------------
    def close(self):
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _dead(self, msg):
        self.close()
        return ProtocolError(msg)

    def _recv(self, n):
        buf = bytearray(n)
        view = memoryview(buf)
        got = 0
        while got < n:
            try:
                r = self._sock.recv_into(view[got:], n - got)
            except OSError:
                self.close()
                raise
            if r == 0:
                self.close()
                raise ConnectionError('zaccel: connection closed by the server')
            got += r
        return buf

    def _xact(self, op, *parts):
        """Send op with payload parts; return the reply length after checking the header.  Raises
        ZaccelError for a nonzero status (payload drained)."""
        if self._sock is None:
            raise ConnectionError('zaccel: connection is closed')
        length = sum(len(p) for p in parts)
        if length > 0xFFFFFFFF:
            raise ValueError('request too large')
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFFFFFFFF
        try:
            self._sock.sendall(struct.pack('<4I', _REQ_MAGIC, op, seq, length))
            for p in parts:
                if len(p):
                    self._sock.sendall(p)
        except OSError:
            self.close()
            raise
        magic, status, rseq, rlen = struct.unpack('<4I', self._recv(16))
        if magic != _REP_MAGIC:
            raise self._dead('bad magic 0x%08x' % magic)
        if rseq != seq:
            raise self._dead('sequence %d, expected %d' % (rseq, seq))
        if status != OK:
            if rlen > _DRAIN_CAP:
                raise self._dead('error reply of %d bytes' % rlen)
            self._recv(rlen)
            raise ZaccelError(status)
        return rlen

    # -- operations -------------------------------------------------------------------------
    def info(self):
        n = self._xact(_OP_INFO)
        if n < 28 or n > 4096:
            raise self._dead('INFO of %d bytes' % n)
        f = struct.unpack_from('<7I', self._recv(n))
        return dict(zip(('version', 'engine', 'mem_total_mb', 'mem_free_mb', 'max_cols',
                         'max_batch', 'selftest'), f))

    def ping(self, data=b''):
        data = bytes(data)
        n = self._xact(_OP_PING, data)
        if n != len(data):
            raise self._dead('PING echo of %d bytes, sent %d' % (n, len(data)))
        if bytes(self._recv(n)) != data:
            raise self._dead('PING echo differs')
        return data

    def load(self, mode, rows, cols, packed):
        rb = row_bytes(mode, cols)
        if rows < 1 or cols < 1:
            raise ValueError('rows and cols must be >= 1')
        packed = bytes(packed) if not isinstance(packed, bytes) else packed
        if len(packed) != rows * rb:
            raise ValueError('packed: expected %d bytes (%d rows x %d), got %d'
                             % (rows * rb, rows, rb, len(packed)))
        n = self._xact(_OP_LOAD, struct.pack('<3I', mode, rows, cols), packed)
        if n != 4:
            raise self._dead('LOAD reply of %d bytes' % n)
        tid, = struct.unpack('<I', self._recv(4))
        return Tensor(tid, mode, rows, cols)

    def free(self, t):
        n = self._xact(_OP_FREE, struct.pack('<I', t.id))
        if n:
            if n > _DRAIN_CAP:
                raise self._dead('FREE reply of %d bytes' % n)
            self._recv(n)

    def gemv(self, t, a, nb=None):
        """a: nb x cols int8 (bytes, flat or nested ints, numpy (cols,) or (nb, cols)).
        Returns (y, cycles, engine_used); y is rows x nb -- a numpy int32 array when a was one,
        else a list of lists."""
        want_np = _is_np(a)
        raw = _int8_bytes(a)
        if nb is None:
            nb = len(raw) // t.cols if t.cols else 0
        if nb < 1 or len(raw) != nb * t.cols:
            raise ValueError('activations: %d bytes is not nb x %d' % (len(raw), t.cols))
        n = self._xact(_OP_GEMV, struct.pack('<2I', t.id, nb), raw)
        if n != 8 + 4 * t.rows * nb:
            raise self._dead('GEMV reply of %d bytes, expected %d' % (n, 8 + 4 * t.rows * nb))
        buf = self._recv(n)
        cycles, engine = struct.unpack_from('<2I', buf)
        if want_np:
            y = np.frombuffer(bytes(buf[8:]), dtype='<i4').astype(np.int32).reshape(t.rows, nb)
        else:
            flat = struct.unpack_from('<%di' % (t.rows * nb), buf, 8)
            y = [list(flat[r * nb:(r + 1) * nb]) for r in range(t.rows)]
        return y, cycles, engine


def connect(host=None, port=None, **kw):
    return Client(host, port, **kw)


def _main(argv):
    host = argv[1] if len(argv) > 1 else None
    port = int(argv[2]) if len(argv) > 2 else None
    try:
        with connect(host, port) as z:
            i = z.info()
            rtt = []
            for _ in range(20):
                t0 = time.perf_counter()
                z.ping(b'x' * 64)
                rtt.append(time.perf_counter() - t0)
            rtt.sort()
            print('zaccel %s:%d  version %d  engine %s  mem %d/%d MB free  max_cols %d  '
                  'max_batch %d  selftest %s' % (z.host, z.port, i['version'],
                                                 'PL' if i['engine'] == 1 else 'CPU-fallback',
                                                 i['mem_free_mb'], i['mem_total_mb'],
                                                 i['max_cols'], i['max_batch'],
                                                 'pass' if i['selftest'] == 0 else
                                                 'FAIL(%d)' % i['selftest']))
            print('PING 64 B round trip, median of 20: %.3f ms' % (rtt[10] * 1e3))
    except (OSError, ZaccelError) as e:
        print('zaccel %s:%s: %s' % (host or default_host(), port or DEFAULT_PORT, e))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(_main(sys.argv))
