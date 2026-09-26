#!/usr/bin/env python3
"""mock_server.py -- a protocol-faithful, CPU-mode zaccel server written from accel/SPEC.md §4.

FOR accel/pi TESTS ONLY.  The real server is accel/zynq/zaccel-server on the Zynq.  This one answers
every op exactly as §4 says, computes GEMV in pure Python (slow: keep test shapes small), and always
reports engine 0 (cpu) with cycles 0.  Nothing it times means anything.

  python3 mock_server.py [--bind 127.0.0.1] [--port 8093|0] [--port-file F] [--mem-mb 384]
                         [--corrupt-every N]

--port 0 picks a free port and writes it to --port-file.  --corrupt-every N adds 1 to the last
result of every Nth GEMV reply, so tests can prove the bench catches a wrong answer.
"""
import argparse
import itertools
import operator
import os
import socket
import socketserver
import struct
import sys
import threading

REQ_MAGIC = 0x3151415A   # "ZAQ1"
REP_MAGIC = 0x3152415A   # "ZAR1"
OK, BADREQ, NOMEM, ENGINE, NOTENSOR = 0, 1, 2, 3, 4
OP_INFO, OP_LOAD, OP_FREE, OP_GEMV, OP_PING = 1, 2, 3, 4, 5
MAX_COLS, MAX_BATCH = 4096, 8
MAX_REQ = 1 << 30

# signed nibble pairs for every byte: (weight 2i from bits 3:0, weight 2i+1 from bits 7:4)
NIB = [((b & 0xF) - 16 if b & 0x8 else b & 0xF, (b >> 4) - 16 if b & 0x80 else b >> 4)
       for b in range(256)]


def row_bytes(mode, cols):
    """SPEC §1: every row starts on a fresh 64-bit beat."""
    return 8 * ((cols + 15) // 16) if mode == 0 else 8 * ((cols + 7) // 8)


class State:
    def __init__(self, mem_mb, corrupt_every):
        self.lock = threading.Lock()
        self.tensors = {}          # id -> (mode, rows, cols, nbytes, [row weights])
        self.next_id = 1
        self.limit = mem_mb << 20
        self.used = 0
        self.corrupt_every = corrupt_every
        self.gemvs = 0


def unpack(mode, rows, cols, data):
    rb = row_bytes(mode, cols)
    out = []
    for r in range(rows):
        seg = data[r * rb:(r + 1) * rb]
        if mode == 0:
            w = list(itertools.chain.from_iterable(NIB[b] for b in seg))[:cols]
        else:
            w = list(struct.unpack('%db' % cols, seg[:cols]))
        out.append(w)
    return out


def op_info(st, p):
    with st.lock:
        free = (st.limit - st.used) >> 20
    return OK, struct.pack('<7I', 1, 0, st.limit >> 20, free, MAX_COLS, MAX_BATCH, 0)


def op_load(st, p):
    if len(p) < 12:
        return BADREQ, b''
    mode, rows, cols = struct.unpack_from('<3I', p)
    if mode not in (0, 1) or rows == 0 or cols == 0 or cols > MAX_COLS:
        return BADREQ, b''
    nbytes = rows * row_bytes(mode, cols)
    if len(p) != 12 + nbytes:
        return BADREQ, b''
    with st.lock:
        if st.used + nbytes > st.limit:
            return NOMEM, b''
        st.used += nbytes
    w = unpack(mode, rows, cols, p[12:])
    with st.lock:
        tid = st.next_id
        st.next_id += 1
        st.tensors[tid] = (mode, rows, cols, nbytes, w)
    return OK, struct.pack('<I', tid)


def op_free(st, p):
    if len(p) != 4:
        return BADREQ, b''
    tid, = struct.unpack('<I', p)
    with st.lock:
        t = st.tensors.pop(tid, None)
        if t is None:
            return NOTENSOR, b''
        st.used -= t[3]
    return OK, b''


def op_gemv(st, p):
    if len(p) < 8:
        return BADREQ, b''
    tid, nb = struct.unpack_from('<2I', p)
    with st.lock:
        t = st.tensors.get(tid)
    if t is None:
        return NOTENSOR, b''
    mode, rows, cols, _, w = t
    if nb < 1 or nb > MAX_BATCH or len(p) != 8 + nb * cols:
        return BADREQ, b''
    acts = [struct.unpack_from('%db' % cols, p, 8 + v * cols) for v in range(nb)]
    y = [sum(map(operator.mul, row, a)) for row in w for a in acts]   # y[r][v], row-major
    with st.lock:
        st.gemvs += 1
        corrupt = st.corrupt_every and st.gemvs % st.corrupt_every == 0
    if corrupt:
        y[-1] += 1
    return OK, struct.pack('<2I', 0, 0) + struct.pack('<%di' % len(y), *y)


OPS = {OP_INFO: op_info, OP_LOAD: op_load, OP_FREE: op_free, OP_GEMV: op_gemv,
       OP_PING: lambda st, p: (OK, bytes(p))}


class Handler(socketserver.BaseRequestHandler):
    def recv_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.request.recv(min(n - len(buf), 1 << 20))
            if not chunk:
                return None
            buf += chunk
        return bytes(buf)

    def handle(self):
        s = self.request
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        st = self.server.state
        while True:
            h = self.recv_exact(16)
            if h is None:
                return
            magic, op, seq, ln = struct.unpack('<4I', h)
            if magic != REQ_MAGIC or ln > MAX_REQ:
                return                      # cannot resynchronise: drop the connection
            p = self.recv_exact(ln) if ln else b''
            if p is None:
                return
            fn = OPS.get(op)
            status, reply = fn(st, p) if fn else (BADREQ, b'')
            s.sendall(struct.pack('<4I', REP_MAGIC, status, seq, len(reply)) + reply)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=8093)
    ap.add_argument('--port-file')
    ap.add_argument('--mem-mb', type=int, default=384)
    ap.add_argument('--corrupt-every', type=int, default=0)
    a = ap.parse_args()
    srv = Server((a.bind, a.port), Handler)
    srv.state = State(a.mem_mb, a.corrupt_every)
    port = srv.server_address[1]
    if a.port_file:
        tmp = a.port_file + '.tmp'
        with open(tmp, 'w') as f:
            f.write('%d\n' % port)
        os.replace(tmp, a.port_file)
    print('mock_server (CPU mode, tests only) on %s:%d mem %d MB%s' %
          (a.bind, port, a.mem_mb, ', corrupting every %d GEMV' % a.corrupt_every
           if a.corrupt_every else ''), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
