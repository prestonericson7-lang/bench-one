#!/usr/bin/env python3
"""golden.py -- independent Python model of SPEC.md section 4 (setup, noclip variant: steps 1-7,
rect, sprite, END) and section 5 (rendering), plus the section 7 collector semantics needed to
turn a single record stream into a list. Used to generate the render-core test scenes.

Usage:
  golden.py gen [outdir]          generate all scenes (default: sim/py_scenes)
  golden.py gen outdir name ...   generate only the named scenes
  golden.py render scene_dir      re-render scene_dir/rec.hex (+ddr.hex) with this model and
                                  compare against scene_dir/expected.hex (cross-check)
  golden.py list                  list scene names

Scene directory format (shared with the other scene generator):
  rec.hex       32-bit words, 8 hex digits per line, records of 24 words, last one END
  ddr.hex       $readmemh image of a 64-bit x 1M-word memory at 0x1E000000 ('@index' lines)
  cfg.txt       clear_color=XXXX, nrec=N, nrender=M (+ this generator's extras: nframes, path,
                expect_overflow, expect_bad, expect_dropped)
  expected.hex  230400 lines x 16 hex digits (64-bit beats, pixel 4k in bits [15:0])
  Multi-frame scenes use ps.hex / teensy.hex stream files (see tb_core.v) and
  expected_<n>.hex per frame.
"""
import os
import random
import struct
import sys

W, H = 1280, 720
REC_WORDS = 24
LIST_SLOTS = 1536
T_NOP, T_TRI, T_SPRITE, T_END = 0, 1, 2, 15
F_ZTEST, F_ZWRITE, F_NOEDGE, F_COLORKEY = 1 << 24, 1 << 25, 1 << 27, 1 << 24
CULL_NONE, CULL_CW, CULL_CCW = 0, 1, 2
DDR_BASE = 0x1E000000
DDR_WORDS = 1 << 20
FB0, FB1, POOL = 0x1E000000, 0x1E200000, 0x1E400000
M32 = 0xFFFFFFFF
INT32_MIN, INT32_MAX = -(1 << 31), (1 << 31) - 1


def u32(v):
    return v & M32


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def f32(x):
    """Round a Python float to IEEE single precision (the C 'float' inputs)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def llrint(x):
    """C llrint with the default rounding mode: round half to even (Python round on floats)."""
    return int(round(x))


def clamp_i32(v):
    return INT32_MIN if v < INT32_MIN else INT32_MAX if v > INT32_MAX else v


def ceil_div(a, b):
    return -((-a) // b)


# ------------------------------------------------------------------------------------------------
# Section 4: setup
# ------------------------------------------------------------------------------------------------
def setup_tri_noclip(v, flags, cull):
    """v = [(x,y,z,r,g,b)] * 3 with float x,y,z (converted to float32) and uint8 colours.
    Returns (1, rec) / (0, None) / (-1, None)."""
    X = [llrint(float(f32(p[0])) * 16.0) for p in v]
    Y = [llrint(float(f32(p[1])) * 16.0) for p in v]
    Zs = []
    for p in v:
        z = f32(p[2])
        z = 0.0 if z < 0.0 else 1.0 if z > 1.0 else z
        Zs.append(float(z) * 65535.0 * 4096.0)
    R = [float(p[3]) * 65536.0 for p in v]
    G = [float(p[4]) * 65536.0 for p in v]
    B = [float(p[5]) * 65536.0 for p in v]
    area2 = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0])
    if area2 == 0:
        return 0, None
    if cull == CULL_CW and area2 > 0:
        return 0, None
    if cull == CULL_CCW and area2 < 0:
        return 0, None
    if area2 < 0:
        for arr in (X, Y, Zs, R, G, B):
            arr[1], arr[2] = arr[2], arr[1]
        area2 = -area2
    for i in range(3):
        if not (-256 * 16 <= X[i] < 1536 * 16 and -256 * 16 <= Y[i] < 976 * 16):
            return -1, None
    minX, maxX, minY, maxY = min(X), max(X), min(Y), max(Y)
    px_min = ceil_div(minX - 8, 16)
    px_max = (maxX - 8) // 16
    py_min = ceil_div(minY - 8, 16)
    py_max = (maxY - 8) // 16
    # "clamp to the screen" read as intersection with [0,1279]x[0,719] (lower bounds raised,
    # upper bounds lowered), so a triangle entirely off one side is empty -> 0. Same as
    # common/gpu_setup.c. (Clamping both ends literally would emit a useless 1-px-wide bbox.)
    px_min = max(px_min, 0)
    px_max = min(px_max, W - 1)
    py_min = max(py_min, 0)
    py_max = min(py_max, H - 1)
    if px_min > px_max or py_min > py_max:
        return 0, None
    rec = [0] * REC_WORDS
    rec[0] = (T_TRI << 28) | (flags & (F_ZTEST | F_ZWRITE)) | (px_max << 11) | px_min
    rec[1] = (py_max << 11) | py_min
    for e, (a, b) in enumerate(((1, 2), (2, 0), (0, 1))):
        A = -16 * (Y[b] - Y[a])
        Bc = 16 * (X[b] - X[a])
        E = (X[b] - X[a]) * (16 * py_min + 8 - Y[a]) - (Y[b] - Y[a]) * (16 * px_min + 8 - X[a])
        if not (A > 0 or (A == 0 and Bc > 0)):
            E -= 1
        rec[2 + 3 * e] = u32(A)
        rec[3 + 3 * e] = u32(Bc)
        rec[4 + 3 * e] = u32(E)
    for k, V in enumerate((Zs, R, G, B)):
        dvdx = 16 * ((V[1] - V[0]) * (Y[2] - Y[0]) - (V[2] - V[0]) * (Y[1] - Y[0])) / area2
        dvdy = 16 * ((V[2] - V[0]) * (X[1] - X[0]) - (V[1] - V[0]) * (X[2] - X[0])) / area2
        vstart = V[0] + dvdx * (16 * px_min + 8 - X[0]) / 16 + dvdy * (16 * py_min + 8 - Y[0]) / 16
        # vstart stored modulo 2^32 (as common/gpu_setup.c does; SPEC 4.6 literally says clamp):
        # the bbox-corner extrapolation may exceed int32 while every covered pixel is in range,
        # and the renderer works modulo 2^32, so wrapping keeps the triangle exact.
        rec[11 + 3 * k] = u32(llrint(vstart))
        rec[12 + 3 * k] = u32(clamp_i32(llrint(dvdx)))
        rec[13 + 3 * k] = u32(clamp_i32(llrint(dvdy)))
    return 1, rec


def setup_rect(x0, y0, x1, y1, rgb565, z, flags):
    bx0, bx1 = max(x0, 0), min(x1, W) - 1
    by0, by1 = max(y0, 0), min(y1, H) - 1
    if bx0 > bx1 or by0 > by1:
        return 0, None
    z = f32(z)
    z = 0.0 if z < 0.0 else 1.0 if z > 1.0 else z
    r5, g6, b5 = (rgb565 >> 11) & 31, (rgb565 >> 5) & 63, rgb565 & 31
    r8, g8, b8 = (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)
    rec = [0] * REC_WORDS
    rec[0] = (T_TRI << 28) | (flags & (F_ZTEST | F_ZWRITE)) | F_NOEDGE | (bx1 << 11) | bx0
    rec[1] = (by1 << 11) | by0
    rec[11] = u32(llrint(float(z) * 65535.0 * 4096.0))
    rec[14] = r8 << 16
    rec[17] = g8 << 16
    rec[20] = b8 << 16
    return 1, rec


def setup_sprite(x, y, w, h, src, stride, ck_en, key):
    if x % 4 or w % 4 or w < 4 or h < 1 or src % 8 or stride % 8:
        return -1, None
    if y < 0:
        src += (-y) * stride
        h += y
        y = 0
    if x < 0:
        src += (-x) * 2
        w += x
        x = 0
    if x >= W or y >= H or w <= 0 or h <= 0:
        return 0, None
    w = min(w, W)
    h = min(h, H)
    rec = [0] * REC_WORDS
    rec[0] = (T_SPRITE << 28) | (F_COLORKEY if ck_en else 0)
    rec[1] = (y << 11) | x
    rec[2] = (h << 11) | w
    rec[3] = u32(src)
    rec[4] = u32(stride)
    rec[5] = key & 0xFFFF
    return 1, rec


def make_end(frame_no):
    rec = [0] * REC_WORDS
    rec[0] = T_END << 28
    rec[1] = u32(frame_no)
    return rec


def make_nop():
    return [0] * REC_WORDS


# ------------------------------------------------------------------------------------------------
# DDR image
# ------------------------------------------------------------------------------------------------
class DDR:
    def __init__(self):
        self.words = {}

    def write16(self, addr, v):
        i = (addr - DDR_BASE) >> 3
        sh = 16 * ((addr >> 1) & 3)
        w = self.words.get(i, 0)
        self.words[i] = (w & ~(0xFFFF << sh)) | ((v & 0xFFFF) << sh)

    def read16(self, addr):
        addr = u32(addr)
        i = (addr - DDR_BASE) >> 3
        if i < 0 or i >= DDR_WORDS:
            raise ValueError('sprite read outside the DDR model: %08x' % addr)
        return (self.words.get(i, 0) >> (16 * ((addr >> 1) & 3))) & 0xFFFF

    def save(self, path):
        with open(path, 'w') as f:
            last = None
            for i in sorted(self.words):
                if last is None or i != last + 1:
                    f.write('@%x\n' % i)
                f.write('%016x\n' % self.words[i])
                last = i

    @staticmethod
    def load(path):
        d = DDR()
        idx = 0
        with open(path) as f:
            for line in f:
                line = line.split('//')[0].strip()
                if not line:
                    continue
                if line.startswith('@'):
                    idx = int(line[1:], 16)
                    continue
                for tok in line.split():
                    d.words[idx] = int(tok, 16)
                    idx += 1
        return d


# ------------------------------------------------------------------------------------------------
# Section 5: rendering
# ------------------------------------------------------------------------------------------------
def render(recs, clear_color, ddr):
    fb = [clear_color & 0xFFFF] * (W * H)
    zb = [0xFFFF] * (W * H)
    for rec in recs:
        t = (rec[0] >> 28) & 15
        if t == T_TRI:
            render_tri(rec, fb, zb)
        elif t == T_SPRITE:
            render_sprite(rec, fb, ddr)
    return fb


def render_tri(rec, fb, zb):
    w0 = rec[0]
    xmin, xmax = w0 & 0x7FF, (w0 >> 11) & 0x7FF
    ymin, ymax = rec[1] & 0x7FF, (rec[1] >> 11) & 0x7FF
    # records are supposed to have the bbox inside the screen; the PL (and this model) clamp
    xe, ye = min(xmax, W - 1), min(ymax, H - 1)
    zt, zw, ne = bool(w0 & F_ZTEST), bool(w0 & F_ZWRITE), bool(w0 & F_NOEDGE)
    A = [rec[2], rec[5], rec[8]]
    B = [rec[3], rec[6], rec[9]]
    E = [rec[4], rec[7], rec[10]]
    z0, dzdx, dzdy = rec[11], rec[12], rec[13]
    ch = [(rec[14], rec[15], rec[16]), (rec[17], rec[18], rec[19]), (rec[20], rec[21], rec[22])]
    for y in range(ymin, ye + 1):
        dy = u32(y - ymin)
        rowbase = y * W
        for x in range(xmin, xe + 1):
            dx = u32(x - xmin)
            if not ne:
                if ((E[0] + A[0] * dx + B[0] * dy) & 0x80000000) or \
                   ((E[1] + A[1] * dx + B[1] * dy) & 0x80000000) or \
                   ((E[2] + A[2] * dx + B[2] * dy) & 0x80000000):
                    continue
            z = s32(z0 + dzdx * dx + dzdy * dy)
            zp = 0 if z < 0 else 65535 if (z >> 12) > 65535 else z >> 12
            idx = rowbase + x
            if zt and not (zp <= zb[idx]):
                continue
            if zw:
                zb[idx] = zp
            c8 = []
            for (c0, cdx, cdy) in ch:
                c = s32(c0 + cdx * dx + cdy * dy)
                c8.append(0 if c < 0 else 255 if (c >> 16) > 255 else c >> 16)
            fb[idx] = ((c8[0] >> 3) << 11) | ((c8[1] >> 2) << 5) | (c8[2] >> 3)


def render_sprite(rec, fb, ddr):
    x, y = rec[1] & 0x7FF, (rec[1] >> 11) & 0x7FF
    w, h = rec[2] & 0x7FF, (rec[2] >> 11) & 0x7FF
    src, stride, key = rec[3], rec[4], rec[5] & 0xFFFF
    ck = bool(rec[0] & F_COLORKEY)
    for row in range(h):
        if y + row >= H:
            break
        for col in range(w):
            if x + col >= W:
                break
            p = ddr.read16(u32(src + row * stride + col * 2))
            if ck and p == key:
                continue
            fb[(y + row) * W + x + col] = p


# ------------------------------------------------------------------------------------------------
# Section 7 (single stream): records -> list
# ------------------------------------------------------------------------------------------------
def collect(words):
    """words: list of (sor, word). Returns (list_records, bad, overflow) up to the first END."""
    lst, bad, ovf = [], 0, 0
    cur, bad_run = None, False
    for sor, wd in words:
        if sor:
            if cur is not None:
                bad += 1
            cur = [wd]
            bad_run = False
        elif cur is not None:
            cur.append(wd)
        else:
            if not bad_run:
                bad += 1
            bad_run = True
            continue
        if len(cur) == REC_WORDS:
            t = (cur[0] >> 28) & 15
            if t in (T_TRI, T_SPRITE):
                if len(lst) < LIST_SLOTS:
                    lst.append(cur)
                else:
                    ovf += 1
            elif t == T_END:
                return lst, bad, ovf, True
            elif t != T_NOP:
                bad += 1
            cur = None
    return lst, bad, ovf, False


# ------------------------------------------------------------------------------------------------
# Output helpers
# ------------------------------------------------------------------------------------------------
def fb_to_hex(fb, path):
    with open(path, 'w') as f:
        out = []
        for i in range(0, W * H, 4):
            out.append('%04x%04x%04x%04x\n' % (fb[i + 3], fb[i + 2], fb[i + 1], fb[i]))
        f.write(''.join(out))


def recs_to_words(recs):
    return [w for r in recs for w in r]


class Stream:
    """tb_core stream file builder (see tb_core.v)."""

    def __init__(self):
        self.e = []

    def push_word(self, sor, w):
        self.e.append((0 << 44) | ((1 if sor else 0) << 32) | u32(w))

    def push_rec(self, rec, nwords=REC_WORDS):
        for i in range(nwords):
            self.push_word(i == 0, rec[i])

    def push_recs(self, recs):
        for r in recs:
            self.push_rec(r)

    def wait_swaps(self, n):
        self.e.append((1 << 44) | n)

    def soft_reset(self):
        self.e.append(2 << 44)

    def control(self, v):
        self.e.append((3 << 44) | v)

    def clear(self, c):
        self.e.append((4 << 44) | c)

    def wait(self, n):
        self.e.append((5 << 44) | n)

    def wait_empty(self):
        self.e.append(6 << 44)

    def ret_enable(self, v):
        self.e.append((7 << 44) | (1 if v else 0))

    def ret_ack(self):
        self.e.append(8 << 44)

    def save(self, path):
        with open(path, 'w') as f:
            for x in self.e + [15 << 44]:
                f.write('%012x\n' % x)


def write_scene(outdir, name, frames, clear_color, ddr=None, recs=None, streams=None,
                path='ps', expect=None, note='', fnos=None, ret_caps=None):
    """frames: list of (list_records, clear_color) actually rendered, in order.
    recs: single-stream record list (rec.hex). streams: dict {'ps': Stream, 'teensy': Stream}."""
    d = os.path.join(outdir, name)
    os.makedirs(d, exist_ok=True)
    for fn in os.listdir(d):
        if fn.endswith('.hex') or fn == 'cfg.txt':
            os.remove(os.path.join(d, fn))
    ddr = ddr or DDR()
    ddr.save(os.path.join(d, 'ddr.hex'))
    if recs is not None:
        with open(os.path.join(d, 'rec.hex'), 'w') as f:
            for w in recs_to_words(recs):
                f.write('%08x\n' % w)
    if streams:
        for k, s in streams.items():
            s.save(os.path.join(d, k + '.hex'))
    cfg = ['clear_color=%04x' % clear_color]
    if recs is not None:
        cfg.append('nrec=%d' % len(recs))
    cfg.append('nrender=%d' % len(frames[-1][0]))
    cfg.append('nframes=%d' % len(frames))
    cfg.append('path=%s' % path)
    for k, v in (expect or {}).items():
        cfg.append('%s=%d' % (k, v))
    if fnos is not None:
        cfg.append('expect_fnos=%s' % ','.join(str(f) for f in fnos))
    if ret_caps is not None:
        cfg.append('ret_caps=%s' % ','.join('%d:%d' % c for c in ret_caps))
    if note:
        cfg.append('note=%s' % note)
    with open(os.path.join(d, 'cfg.txt'), 'w') as f:
        f.write('\n'.join(cfg) + '\n')
    for n, (lst, cc) in enumerate(frames):
        fb = render(lst, cc, ddr)
        fn = 'expected.hex' if len(frames) == 1 else 'expected_%d.hex' % n
        fb_to_hex(fb, os.path.join(d, fn))
    print('scene %-14s frames=%d list=%s' % (name, len(frames), [len(f[0]) for f in frames]))


# ------------------------------------------------------------------------------------------------
# Scene helpers
# ------------------------------------------------------------------------------------------------
def tri(v, flags=F_ZTEST | F_ZWRITE, cull=CULL_NONE):
    r, rec = setup_tri_noclip(v, flags, cull)
    return rec if r == 1 else None


def add(lst, rec):
    if rec is not None:
        lst.append(rec)


def rand_vtx(rng, x0, x1, y0, y1):
    return (rng.uniform(x0, x1), rng.uniform(y0, y1), rng.uniform(0.0, 1.0),
            rng.randrange(256), rng.randrange(256), rng.randrange(256))


def sprite_pool(ddr, rng, base, w, h, stride, key, key_frac=0.2):
    """Fill a w x h RGB565 image at `base` with `stride` bytes per row; ~key_frac pixels = key."""
    for r in range(h):
        for c in range(w):
            if rng.random() < key_frac:
                p = key
            else:
                p = rng.randrange(65536)
                if p == key:
                    p ^= 1
            ddr.write16(base + r * stride + c * 2, p)


# ------------------------------------------------------------------------------------------------
# Scenes
# ------------------------------------------------------------------------------------------------
def sc_single_tri(out):
    lst = []
    add(lst, tri([(200.3, 90.7, 0.25, 255, 0, 0), (1000.9, 300.2, 0.5, 0, 255, 0),
                  (420.5, 610.1, 0.75, 0, 0, 255)]))
    write_scene(out, 'single_tri', [(lst, 0x0010)], 0x0010, recs=lst + [make_end(0)])


def sc_rects(out):
    lst = []
    add(lst, setup_rect(-50, -50, 2000, 2000, 0x1234, 0.9, F_ZTEST | F_ZWRITE)[1])   # full screen
    add(lst, setup_rect(0, 0, 1, 1, 0xFFFF, 0.1, 0)[1])                               # 1 px corner
    add(lst, setup_rect(1279, 719, 1280, 720, 0xF800, 0.1, 0)[1])                     # 1 px corner
    add(lst, setup_rect(1277, 5, 1400, 40, 0x07E0, 0.5, F_ZTEST | F_ZWRITE)[1])      # right clip
    add(lst, setup_rect(3, 700, 9, 800, 0x001F, 0.5, F_ZTEST | F_ZWRITE)[1])         # bottom clip
    add(lst, setup_rect(101, 13, 102, 250, 0xAAAA, 0.3, F_ZTEST)[1])                  # 1 px column
    add(lst, setup_rect(50, 30, 700, 31, 0x5555, 0.3, F_ZTEST)[1])                    # 1 px row
    # z-tested overlap: nearer rect first, farther second (must be hidden), equal z passes
    add(lst, setup_rect(300, 200, 500, 400, 0xF81F, 0.2, F_ZTEST | F_ZWRITE)[1])
    add(lst, setup_rect(350, 250, 600, 450, 0xFFE0, 0.6, F_ZTEST | F_ZWRITE)[1])
    add(lst, setup_rect(420, 150, 460, 420, 0x07FF, 0.2, F_ZTEST)[1])                 # equal z
    # no ZTEST: always drawn, ZWRITE updates z to far, then a mid-z rect passes over it
    add(lst, setup_rect(800, 100, 900, 600, 0x0F0F, 1.0, F_ZWRITE)[1])
    add(lst, setup_rect(780, 300, 1000, 350, 0xF0F0, 0.95, F_ZTEST)[1])
    for i in range(40):                                                              # alignments
        x0 = 600 + i * 3 + (i % 4)
        add(lst, setup_rect(x0, 500 + i, x0 + 1 + (i % 7), 520 + 2 * i, (i * 1571) & 0xFFFF, 0.05, 0)[1])
    write_scene(out, 'rects', [(lst, 0x0000)], 0x0000, recs=lst + [make_end(1)])


def sc_zbuf(out):
    rng = random.Random(7)
    lst = []
    # back-to-front
    add(lst, tri([(100, 100, 0.8, 255, 0, 0), (500, 120, 0.8, 255, 0, 0), (300, 500, 0.8, 255, 0, 0)]))
    add(lst, tri([(150, 90, 0.2, 0, 255, 0), (450, 400, 0.9, 0, 255, 0), (120, 450, 0.5, 0, 255, 0)]))
    # front-to-back (second must be mostly hidden)
    add(lst, tri([(700, 100, 0.1, 0, 0, 255), (1100, 150, 0.3, 0, 0, 255), (800, 500, 0.2, 0, 0, 255)]))
    add(lst, tri([(650, 80, 0.5, 255, 255, 0), (1200, 300, 0.05, 255, 255, 0), (700, 600, 0.9, 255, 255, 0)]))
    # ZTEST without ZWRITE, ZWRITE without ZTEST
    add(lst, tri([(100, 520, 0.3, 200, 200, 200), (600, 540, 0.3, 200, 200, 200), (300, 710, 0.3, 200, 200, 200)],
                 F_ZWRITE))
    add(lst, tri([(120, 500, 0.6, 50, 50, 250), (580, 560, 0.1, 50, 50, 250), (310, 715, 0.2, 50, 50, 250)],
                 F_ZTEST))
    # hazard test: many consecutive tiny overlapping triangles over the same pixels / Z words
    for i in range(120):
        cx, cy = 900 + (i % 5), 620 + (i % 3)
        z = rng.uniform(0.0, 1.0)
        add(lst, tri([(cx - 3.3, cy - 2.1, z, rng.randrange(256), rng.randrange(256), rng.randrange(256)),
                      (cx + 4.7, cy - 1.2, z * 0.9, 255, 0, 0),
                      (cx + 0.4, cy + 5.6, 1.0 - z, 0, 255, 0)], F_ZTEST | F_ZWRITE))
    # coplanar interpenetrating
    add(lst, tri([(900, 100, 0.0, 255, 0, 0), (1200, 100, 1.0, 255, 0, 0), (1050, 400, 0.5, 255, 0, 0)]))
    add(lst, tri([(900, 100, 1.0, 0, 255, 0), (1200, 100, 0.0, 0, 255, 0), (1050, 400, 0.5, 0, 255, 0)]))
    write_scene(out, 'zbuf', [(lst, 0x0841)], 0x0841, recs=lst + [make_end(2)])


def sc_sprite(out):
    rng = random.Random(11)
    ddr = DDR()
    key = 0xF81F
    lst = []
    add(lst, tri([(0, 0, 0.5, 255, 0, 0), (1279.9, 0, 0.5, 0, 255, 0), (640, 719.9, 0.5, 0, 0, 255)]))
    # A: 64x40 at (100,50), colour key, stride 136 (not a multiple of 128)
    a_src, a_str = POOL + 0x0F80, 136          # row 0 starts 128 bytes before a 4 KB boundary
    sprite_pool(ddr, rng, a_src, 64, 40, a_str, key)
    add(lst, setup_sprite(100, 50, 64, 40, a_src, a_str, True, key)[1])
    # B: right/bottom clipped (1240 + 80 > 1280, 700 + 60 > 720), no key
    b_src, b_str = POOL + 0x10000 + 0x0FF8, 160   # source bursts cross 4 KB boundaries
    sprite_pool(ddr, rng, b_src, 80, 60, b_str, key)
    add(lst, setup_sprite(1240, 700, 80, 60, b_src, b_str, False, key)[1])
    # C: negative x/y (setup clips by advancing src), key
    c_src, c_str = POOL + 0x20000 + 8, 256
    sprite_pool(ddr, rng, c_src, 48, 48, c_str, key, 0.5)
    add(lst, setup_sprite(-16, -9, 48, 48, c_src, c_str, True, key)[1])
    # D: wide (600 px), 1 row tall and 4-pixel narrow tall, crossing strips
    d_src = POOL + 0x30000 + 0x0F00
    sprite_pool(ddr, rng, d_src, 600, 1, 1200, key)
    add(lst, setup_sprite(400, 333, 600, 1, d_src, 1200, True, key)[1])
    e_src = POOL + 0x40000 + 0xFF0
    sprite_pool(ddr, rng, e_src, 4, 200, 8, key)
    add(lst, setup_sprite(1276, 300, 4, 200, e_src, 8, True, key)[1])
    # a triangle drawn over sprite A (ZTEST passes: nearer), then sprite F over the triangle
    add(lst, tri([(90, 40, 0.1, 255, 255, 255), (200, 70, 0.1, 0, 0, 0), (110, 120, 0.1, 128, 128, 128)]))
    f_src, f_str = POOL + 0x50000, 64
    sprite_pool(ddr, rng, f_src, 32, 32, f_str, key, 0.3)
    add(lst, setup_sprite(120, 60, 32, 32, f_src, f_str, True, key)[1])
    # G: full-width sprite rows (1280 px) over part of the screen, stride = 2560
    g_src = POOL + 0x100000
    sprite_pool(ddr, rng, g_src, 1280, 20, 2560, key, 0.05)
    add(lst, setup_sprite(0, 395, 1280, 20, g_src, 2560, True, key)[1])
    write_scene(out, 'sprite_ckey', [(lst, 0x2104)], 0x2104, ddr=ddr, recs=lst + [make_end(3)],
                path='teensy')


def sc_strip_cross(out):
    rng = random.Random(3)
    lst = []
    # full-height tall thin triangles at every x alignment
    for i in range(8):
        x = 30 + i * 37.25 + i * 0.25
        add(lst, tri([(x, 0.2, 0.5, 255, 128, i * 30), (x + 2.6 + i * 0.5, 719.8, 0.5, 0, 255, 0),
                      (x - 1.5, 719.5, 0.5, 0, 0, 255)], 0))
    # triangles starting/ending mid-strip, covering 1..3 strips
    for i in range(30):
        y0 = 13 + i * 23.37
        x0 = 400 + (i % 10) * 70 + (i % 4) * 0.25
        add(lst, tri([(x0, y0, 0.3, 255, 0, 0), (x0 + 50.5, y0 + 3.1, 0.4, 0, 255, 0),
                      (x0 + 20.2, y0 + 17 + i * 1.3, 0.7, 0, 0, 255)], F_ZTEST | F_ZWRITE))
    # edges exactly on pixel centres / strip boundaries (top-left rule)
    add(lst, tri([(1100.5, 15.5, 0.2, 255, 255, 255), (1200.5, 15.5, 0.2, 255, 255, 255),
                  (1100.5, 48.5, 0.2, 255, 255, 255)], 0))
    add(lst, tri([(1200.5, 15.5, 0.2, 255, 0, 255), (1200.5, 48.5, 0.2, 255, 0, 255),
                  (1100.5, 48.5, 0.2, 255, 0, 255)], 0))
    # touching the right and bottom screen edge, beyond the edge (guard band)
    add(lst, tri([(1250, 650, 0.1, 10, 20, 30), (1500, 700, 0.1, 200, 100, 50), (1260, 900, 0.1, 90, 180, 250)]))
    add(lst, tri([(-200, -100, 0.9, 255, 255, 0), (300, -50, 0.1, 0, 255, 255), (-100, 200, 0.5, 255, 0, 255)]))
    # huge triangle behind everything
    add(lst, tri([(-250, -250, 0.99, 30, 30, 30), (1530, -200, 0.99, 60, 60, 60), (600, 970, 0.99, 90, 90, 90)]))
    # random small/medium triangles
    for i in range(60):
        cx, cy = rng.uniform(0, W), rng.uniform(0, H)
        s = rng.uniform(1, 60)
        add(lst, tri([rand_vtx(rng, cx - s, cx + s, cy - s, cy + s) for _ in range(3)],
                     rng.choice([0, F_ZTEST, F_ZWRITE, F_ZTEST | F_ZWRITE])))
    write_scene(out, 'strip_cross', [(lst, 0x0000)], 0x0000, recs=lst + [make_end(4)])


def sc_two_frames(out):
    rng = random.Random(5)
    t0, p0, t1, p1 = [], [], [], []
    for i in range(20):
        add(t0, tri([rand_vtx(rng, 0, 640, 0, 720) for _ in range(3)]))
        add(t1, tri([rand_vtx(rng, 640, 1280, 0, 720) for _ in range(3)]))
    for i in range(10):
        add(p0, setup_rect(i * 60, 10 + i * 5, i * 60 + 40, 60 + i * 5, 0xF800 | i, 0.0, 0)[1])
        add(p1, setup_rect(i * 60, 600 + i * 5, i * 60 + 40, 650 + i * 5, 0x07E0 | i, 0.0, 0)[1])
    ts, ps = Stream(), Stream()
    ps.control(6)                        # SRC_TEENSY | SRC_PS
    ps.clear(0x0010)
    # Teensy sends both frames as fast as it can (frame-1 records sit behind the END barrier)
    ts.push_recs(t0 + [make_end(1000)] + t1 + [make_end(1001)])
    # PS: frame 0, then waits for frame 0 to be displayed before changing the clear colour
    ps.push_recs(p0 + [make_end(2000)])
    ps.wait_swaps(1)
    ps.clear(0x8010)
    ps.push_recs(p1 + [make_end(2001)])
    # frame_no of a list = the PS END's w1 when SRC_PS is enabled (SPEC 13.1)
    write_scene(out, 'two_frames', [(t0 + p0, 0x0010), (t1 + p1, 0x8010)], 0x0010,
                streams={'ps': ps, 'teensy': ts}, path='both',
                expect={'expect_last_fno': 2001}, fnos=[2000, 2001])


def sc_overflow(out):
    lst = []
    for i in range(LIST_SLOTS + 4):
        x, y = (i % 64) * 20, (i // 64) * 28
        lst.append(setup_rect(x, y, x + 12 + (i % 5), y + 9 + (i % 7), (i * 2654435761) & 0xFFFF,
                              0.5, 0)[1])
    words = [(j % REC_WORDS == 0, w) for j, w in enumerate(recs_to_words(lst + [make_end(5)]))]
    rl, bad, ovf, ok = collect(words)
    assert ok and ovf == 4 and len(rl) == LIST_SLOTS
    write_scene(out, 'overflow', [(rl, 0x0000)], 0x0000, recs=lst + [make_end(5)],
                expect={'expect_overflow': 4})


def sc_empty(out):
    ps = Stream()
    ps.control(4)
    ps.clear(0x001F)
    ps.push_rec(make_end(0))
    ps.wait_swaps(1)
    ps.clear(0xFFE0)                     # new clear colour: banks must be re-cleared
    ps.push_rec(make_end(1))
    ps.wait_swaps(2)
    ps.push_rec(make_end(2))             # same colour again
    write_scene(out, 'empty', [([], 0x001F), ([], 0xFFE0), ([], 0xFFE0)], 0x001F, streams={'ps': ps},
                fnos=[0, 1, 2], expect={'expect_last_fno': 2})


def sc_ret_capture(out):
    """SPEC 13.1 return capture: capture when enabled and not full, RET_FULL blocks the next
    capture until RET_ACK, disabling mid-capture lets the capture complete, frame numbers."""
    rng = random.Random(31)
    frames = []
    for f in range(4):
        lst = []
        for i in range(6):
            cx, cy = rng.uniform(100, 1180), rng.uniform(100, 620)
            add(lst, tri([rand_vtx(rng, cx - 120, cx + 120, cy - 120, cy + 120) for _ in range(3)]))
        add(lst, setup_rect(40 + 200 * f, 40, 140 + 200 * f, 140, 0xFFFF, 0.0, 0)[1])  # frame marker
        frames.append(lst)
    ps = Stream()
    ps.control(4)
    ps.clear(0x0000)
    ps.ret_enable(1)
    ps.push_recs(frames[0] + [make_end(100)])    # captured -> RET_FULL
    ps.wait_swaps(1)
    ps.push_recs(frames[1] + [make_end(101)])    # not captured (RET_FULL still 1)
    ps.wait_swaps(2)
    ps.ret_ack()                                 # release the buffer
    ps.push_recs(frames[2] + [make_end(102)])    # captured
    ps.wait(3000)
    ps.ret_enable(0)                             # mid-capture: the capture still completes
    ps.wait_swaps(3)
    ps.ret_ack()
    ps.push_recs(frames[3] + [make_end(103)])    # not captured (RET_ENABLE = 0)
    write_scene(out, 'ret_capture', [(f, 0x0000) for f in frames], 0x0000, streams={'ps': ps},
                fnos=[100, 101, 102, 103], ret_caps=[(0, 100), (2, 102)],
                expect={'expect_last_fno': 103})


def sc_soft_reset(out):
    rng = random.Random(9)
    f0, junk, c = [], [], []
    for i in range(40):
        cx, cy = rng.uniform(0, W), rng.uniform(0, H)
        add(f0, tri([rand_vtx(rng, cx - 150, cx + 150, cy - 150, cy + 150) for _ in range(3)]))
    for i in range(6):
        add(junk, setup_rect(0, i * 100, 1280, i * 100 + 50, 0xFFFF, 0.0, 0)[1])
    add(c, tri([(100, 100, 0.5, 255, 0, 0), (1100, 200, 0.5, 0, 255, 0), (600, 650, 0.5, 0, 0, 255)]))
    add(c, setup_rect(10, 10, 60, 60, 0xF800, 0.0, F_ZTEST)[1])
    ps = Stream()
    ps.control(4)
    ps.clear(0x0000)
    ps.push_recs(f0 + [make_end(0)])     # frame 0 (renders while the rest happens)
    ps.push_recs(junk[:3] + [make_end(1)])   # a complete list waiting behind frame 0
    ps.wait_empty()
    # frame 0 must have started rendering: after reset the core first resets its Z-bank tags
    # (one pass over the Z bank, 2560 cycles), so the first list starts ~2.6k cycles after reset
    ps.wait(8000)
    ps.soft_reset()                      # discards the waiting list; frame 0 keeps rendering
    ps.push_recs(junk[3:5])
    ps.push_rec(junk[5], 11)             # partial record (11 words)
    ps.wait_empty()
    ps.wait(50)
    ps.soft_reset()                      # soft reset mid-record
    ps.push_recs(c + [make_end(1)])
    write_scene(out, 'soft_reset', [(f0, 0x0000), (c, 0x0000)], 0x0000, streams={'ps': ps},
                expect={'expect_bad': 0}, fnos=[0, 1])


def sc_bad_records(out):
    rng = random.Random(13)
    good = []
    for i in range(10):
        add(good, tri([rand_vtx(rng, 0, 1280, 0, 720) for _ in range(3)]))
    ps, ts = Stream(), Stream()
    ps.control(4)                        # PS only: Teensy words must be drained and dropped
    ps.clear(0x4208)
    for i in range(100):
        ts.push_word(i % 24 == 0, rng.randrange(1 << 32))
    ps.push_rec(good[0])
    ps.push_rec(good[1], 7)              # partial, interrupted by a new sor=1 word -> bad 1
    ps.push_rec(good[2])
    for i in range(30):                  # run of sor=0 garbage -> bad 2 (once)
        ps.push_word(False, rng.randrange(1 << 32))
    unk = list(good[3])
    unk[0] = (unk[0] & 0x0FFFFFFF) | (5 << 28)
    ps.push_rec(unk)                     # unknown type -> bad 3
    ps.push_rec(make_nop())              # NOP ignored
    ps.push_recs(good[3:])
    ps.push_rec(make_end(0))
    lst = [good[0], good[2]] + good[3:]
    write_scene(out, 'bad_records', [(lst, 0x4208)], 0x4208, streams={'ps': ps, 'teensy': ts},
                path='ps', expect={'expect_bad': 3, 'expect_dropped': 100})


def sc_src_switch(out):
    """CONTROL source bits changed while a record is open.
    Frame 0: Teensy+PS; 10 Teensy words of a record are consumed, then CONTROL = PS only ->
    the partial Teensy record is discarded (bad 1); later Teensy words are drained (dropped 24).
    Frame 1: Teensy+PS; Teensy delivers its part + END, a PS record is half received, then
    CONTROL = Teensy only -> the list completes, the partial PS record is discarded (bad 2),
    frame_no = the Teensy END (SRC_PS disabled at completion)."""
    rng = random.Random(37)
    p0, t1 = [], []
    for i in range(5):
        add(p0, tri([rand_vtx(rng, 0, 1280, 0, 360) for _ in range(3)]))
        add(t1, tri([rand_vtx(rng, 0, 1280, 360, 720) for _ in range(3)]))
    ts, ps = Stream(), Stream()
    ts.control(6)
    ts.push_rec(t1[0], 10)               # partial Teensy record
    ts.wait_empty()
    ts.wait(20)
    ts.control(4)                        # PS only: open Teensy record discarded
    for i in range(24):
        ts.push_word(i == 0, rng.randrange(1 << 32))   # drained
    ps.push_recs(p0 + [make_end(500)])
    ts.wait_swaps(1)
    ts.control(6)
    ts.push_recs(t1 + [make_end(600)])
    ps.wait_swaps(1)
    ps.wait(60)
    ps.push_rec(p0[0], 7)                # partial PS record (read after the Teensy END)
    ps.wait_empty()
    ps.wait(20)
    ps.control(2)                        # Teensy only: list completes, PS partial discarded
    write_scene(out, 'src_switch', [(p0, 0x0000), (t1, 0x0000)], 0x0000,
                streams={'ps': ps, 'teensy': ts}, path='both',
                expect={'expect_bad': 2, 'expect_dropped': 24, 'expect_last_fno': 600},
                fnos=[500, 600])


def sc_edge_records(out):
    """Raw records at the edges of the contract: bboxes partly/fully outside the screen,
    xmin > xmax, ymin > ymax, single-pixel corner records, a full-height 4-pixel sprite at the
    right edge, a sprite taller than the screen, and an empty-range sprite (y >= 720)."""
    rng = random.Random(41)
    ddr = DDR()
    lst = []

    def raw_tri(x0, x1, y0, y1, flags=0, col=(255, 255, 255)):
        rec = [0] * REC_WORDS
        rec[0] = (T_TRI << 28) | flags | F_NOEDGE | (x1 << 11) | x0
        rec[1] = (y1 << 11) | y0
        rec[11] = 0x10000000
        rec[14], rec[17], rec[20] = col[0] << 16, col[1] << 16, col[2] << 16
        rec[15] = u32(-(1 << 12))            # red gradient along x, clamps at 0
        rec[19] = 1 << 13                     # green gradient along y, clamps at 255
        return rec
    lst.append(raw_tri(1270, 2047, 700, 2047))                  # extends past both edges
    lst.append(raw_tri(1280, 1300, 10, 20))                     # xmin >= 1280: nothing
    lst.append(raw_tri(10, 20, 720, 730))                       # ymin >= 720: nothing
    lst.append(raw_tri(50, 40, 10, 20))                         # xmin > xmax: nothing
    lst.append(raw_tri(10, 20, 30, 25))                         # ymin > ymax: nothing
    lst.append(raw_tri(1279, 1279, 719, 719, 0, (0, 0, 255)))   # bottom-right pixel
    lst.append(raw_tri(0, 0, 0, 0, 0, (255, 0, 0)))             # top-left pixel
    lst.append(raw_tri(0, 1279, 15, 16, 0, (0, 255, 0)))        # 2 rows across a strip border
    lst.append(raw_tri(3, 4, 100, 700, 0, (255, 0, 255)))       # 2 columns across a group border
    key = 0x1234
    src = POOL + 0x2000
    sprite_pool(ddr, rng, src, 4, 720, 8, key)
    add(lst, setup_sprite(1276, 0, 4, 720, src, 8, True, key)[1])   # full height, right edge
    src2 = POOL + 0x8000
    sprite_pool(ddr, rng, src2, 8, 40, 16, key)
    rec = setup_sprite(600, 700, 8, 40, src2, 16, False, key)[1]    # rows 700..739 -> clipped
    lst.append(rec)
    bad = list(rec)
    bad[1] = (720 << 11) | 600                                      # y = 720: nothing
    lst.append(bad)
    write_scene(out, 'edge_records', [(lst, 0x0000)], 0x0000, ddr=ddr, recs=lst + [make_end(9)])


def sc_random_raw(out):
    """Random raw TRI records: arbitrary 32-bit A/B/E/gradients (wrap-around arithmetic, clamps),
    random flags, small random bboxes (some extending past the screen edge)."""
    rng = random.Random(17)
    lst = []
    for i in range(150):
        x0, y0 = rng.randrange(0, 1280), rng.randrange(0, 720)
        x1 = min(2047, x0 + rng.randrange(0, 40))
        y1 = min(2047, y0 + rng.randrange(0, 40))
        flags = rng.choice([0, F_ZTEST, F_ZWRITE, F_ZTEST | F_ZWRITE]) | (F_NOEDGE if rng.random() < 0.2 else 0)
        rec = [0] * REC_WORDS
        rec[0] = (T_TRI << 28) | flags | (x1 << 11) | x0
        rec[1] = (y1 << 11) | y0
        for k in range(2, 23):
            mode = rng.randrange(4)
            if mode == 0:
                rec[k] = rng.randrange(1 << 32)
            elif mode == 1:
                rec[k] = u32(rng.randrange(-(1 << 20), 1 << 20))
            elif mode == 2:
                rec[k] = u32(rng.randrange(-(1 << 26), 1 << 26))
            else:
                rec[k] = u32(rng.randrange(-(1 << 31), 1 << 31))
        lst.append(rec)
    write_scene(out, 'random_raw', [(lst, 0x0000)], 0x0000, recs=lst + [make_end(6)])


def sc_random_mix(out, name='random_mix', seed=21, ntri=250, path='ps'):
    rng = random.Random(seed)
    ddr = DDR()
    key = 0x07E0
    spr = []
    for s in range(4):
        w, h = 4 * rng.randrange(1, 40), rng.randrange(1, 90)
        stride = 8 * rng.randrange((w * 2 + 7) // 8, (w * 2 + 7) // 8 + 20)
        src = POOL + 0x40000 * s + 8 * rng.randrange(0, 1024)
        sprite_pool(ddr, rng, src, w, h, stride, key, 0.25)
        spr.append((w, h, stride, src))
    lst = []
    for i in range(ntri):
        r = rng.random()
        if r < 0.75:
            cx, cy = rng.uniform(-100, 1380), rng.uniform(-100, 820)
            s = rng.choice([3, 10, 40, 150, 500])
            add(lst, tri([rand_vtx(rng, cx - s, cx + s, cy - s, cy + s) for _ in range(3)],
                         rng.choice([0, F_ZTEST, F_ZWRITE, F_ZTEST | F_ZWRITE]),
                         rng.choice([CULL_NONE, CULL_NONE, CULL_CW, CULL_CCW])))
        elif r < 0.9:
            x0, y0 = rng.randrange(-50, 1300), rng.randrange(-50, 740)
            add(lst, setup_rect(x0, y0, x0 + rng.randrange(1, 300), y0 + rng.randrange(1, 200),
                                rng.randrange(65536), rng.uniform(-0.2, 1.2),
                                rng.choice([0, F_ZTEST, F_ZWRITE, F_ZTEST | F_ZWRITE]))[1])
        else:
            w, h, stride, src = rng.choice(spr)
            x = 4 * rng.randrange(-10, 330)
            y = rng.randrange(-50, 740)
            add(lst, setup_sprite(x, y, w, h, src, stride, rng.random() < 0.7, key)[1])
    write_scene(out, name, [(lst, 0x18E3)], 0x18E3, ddr=ddr, recs=lst + [make_end(seed)], path=path,
                fnos=[seed])


def sc_perf_mesh(out):
    """A 40x18 grid of quads (1440 triangles) covering the screen, Gouraud, Z on: typical load."""
    rng = random.Random(23)
    lst = []
    nx, ny = 40, 18
    pts = {}
    for j in range(ny + 1):
        for i in range(nx + 1):
            jx = rng.uniform(-6, 6) if 0 < i < nx else 0
            jy = rng.uniform(-6, 6) if 0 < j < ny else 0
            pts[i, j] = (i * 32 + jx, j * 40 + jy, rng.uniform(0.2, 0.8),
                         rng.randrange(256), rng.randrange(256), rng.randrange(256))
    for j in range(ny):
        for i in range(nx):
            a, b, c, d = pts[i, j], pts[i + 1, j], pts[i + 1, j + 1], pts[i, j + 1]
            add(lst, tri([a, b, c]))
            add(lst, tri([a, c, d]))
    write_scene(out, 'perf_mesh', [(lst, 0x0000)], 0x0000, recs=lst + [make_end(8)])


def ring_list(rng, n, tag):
    """n small records (TRI / rect / few SPRITE-free) in a grid, all flags, for the ring scenes."""
    lst = []
    i = 0
    while len(lst) < n:
        gx, gy = (i * 7 + tag * 13) % 64, (i * 3 + tag * 5) % 36
        cx, cy = gx * 20 + 10, gy * 20 + 10
        r = rng.random()
        if r < 0.7:
            rec = tri([rand_vtx(rng, cx - 14, cx + 14, cy - 14, cy + 14) for _ in range(3)],
                      rng.choice([0, F_ZTEST, F_ZWRITE, F_ZTEST | F_ZWRITE]))
        else:
            rec = setup_rect(cx - 9, cy - 9, cx + rng.randrange(1, 12), cy + rng.randrange(1, 12),
                             rng.randrange(65536), rng.uniform(0.0, 1.0),
                             rng.choice([0, F_ZTEST, F_ZWRITE, F_ZTEST | F_ZWRITE]))[1]
        if rec is not None:
            lst.append(rec)
        i += 1
    return lst


def sc_ring_full(out):
    """Record ring (2048 slots shared by the two lists) back-pressure and wrap-around: four
    lists of 1536, 1536, 1000 and 700 records pushed back to back through one FIFO. List 1 can
    only take 511 slots while list 0 renders (the collector stalls until list 0 is freed);
    list 1 occupies slots 1536..3071 (wraps at 2048), list 3 slots 2024..2723 (wraps)."""
    rng = random.Random(61)
    sizes = [1536, 1536, 1000, 700]
    lists = [ring_list(rng, n, k) for k, n in enumerate(sizes)]
    ps = Stream()
    ps.control(4)
    ps.clear(0x0000)
    for k, lst in enumerate(lists):
        ps.push_recs(lst + [make_end(700 + k)])
    write_scene(out, 'ring_full', [(lst, 0x0000) for lst in lists], 0x0000, streams={'ps': ps},
                fnos=[700 + k for k in range(4)], expect={'expect_last_fno': 703, 'expect_bad': 0,
                                                          'expect_overflow': 0,
                                                          'expect_ring_stall': 1})


def sc_ring_srst(out):
    """SOFT_RESET while the ring is full: list 0 (1536 records) renders, list 1 has taken the
    511 usable slots and 16 more records wait in the FIFO; the soft reset discards list 1 and the
    FIFO. The next list must start right after list 0 in the ring (slots 1536.., wrapping), or it
    would overwrite list 0 while list 0 is still being rendered."""
    rng = random.Random(67)
    l0 = ring_list(rng, 1536, 1)
    junk = ring_list(rng, 527, 2)
    l1 = ring_list(rng, 1536, 3)
    ps = Stream()
    ps.control(4)
    ps.clear(0x0010)
    ps.push_recs(l0 + [make_end(40)])
    ps.push_recs(junk)                   # 511 fit in the ring, 16 stay in the FIFO
    ps.wait(3000)
    ps.soft_reset()
    ps.push_recs(l1 + [make_end(41)])
    write_scene(out, 'ring_srst', [(l0, 0x0010), (l1, 0x0010)], 0x0010, streams={'ps': ps},
                fnos=[40, 41], expect={'expect_last_fno': 41, 'expect_bad': 0,
                                       'expect_overflow': 0, 'expect_ring_stall': 1})


def sc_zclear_frames(out):
    """Single tagged Z bank: Z must read as 0xFFFF at the start of every frame and of every
    strip. f0 writes z=0 everywhere and then fails a z=0.5 ZTEST triangle; f1 draws a z=1.0
    (Z16 = 65535) ZTEST rect over the whole screen, visible only if Z was reset; f2 writes z=0.3
    in rows 0..15 and 40..47 only, then a z=0.6 ZTEST rect over rows 0..63 is visible in rows
    16..39 and 48..63 (same Z words as rows 0..15 / 32..47, other strip)."""
    f0, f1, f2 = [], [], []
    add(f0, setup_rect(0, 0, 1280, 720, 0xF800, 0.0, F_ZTEST | F_ZWRITE)[1])
    add(f0, tri([(100, 100, 0.5, 0, 255, 0), (1200, 150, 0.5, 0, 255, 0), (600, 700, 0.5, 0, 255, 0)]))
    add(f1, setup_rect(0, 0, 1280, 720, 0x07E0, 1.0, F_ZTEST | F_ZWRITE)[1])
    add(f1, tri([(50, 60, 0.5, 255, 0, 0), (1250, 100, 0.2, 0, 0, 255), (700, 710, 0.9, 255, 255, 0)]))
    add(f1, setup_rect(200, 200, 400, 300, 0xFFFF, 0.99, F_ZTEST)[1])
    add(f2, setup_rect(0, 0, 1280, 16, 0x001F, 0.3, F_ZTEST | F_ZWRITE)[1])
    add(f2, setup_rect(0, 40, 1280, 48, 0x001F, 0.3, F_ZTEST | F_ZWRITE)[1])
    add(f2, setup_rect(0, 0, 1280, 64, 0xFFE0, 0.6, F_ZTEST | F_ZWRITE)[1])
    add(f2, tri([(10, 5, 0.1, 255, 255, 255), (1270, 30, 0.1, 255, 0, 255), (640, 700, 0.95, 0, 255, 255)],
                F_ZTEST))
    ps = Stream()
    ps.control(4)
    ps.clear(0x0000)
    ps.push_recs(f0 + [make_end(1)])
    ps.wait_swaps(1)
    ps.clear(0x1111)
    ps.push_recs(f1 + [make_end(2)])
    ps.wait_swaps(2)
    ps.push_recs(f2 + [make_end(3)])
    write_scene(out, 'zclear_frames', [(f0, 0x0000), (f1, 0x1111), (f2, 0x1111)], 0x0000,
                streams={'ps': ps}, fnos=[1, 2, 3], expect={'expect_last_fno': 3})


def sc_ret_capture2(out):
    """SPEC 13.1 corner cases, two sources:
      f0 PS only: RET_ACK while not full (nothing), RET_ENABLE set only after f0 started -> no
         capture;
      f1 PS only: captured; RET_ACK during the capture (not full yet -> nothing);
      f2 PS only: not captured (RET_FULL);
      f3 Teensy + PS: RET_ACK then RET_ENABLE 0 -> 1 before it starts -> captured,
         RET_FRAME = the PS END (SRC_PS enabled);
      f4 Teensy only (CONTROL = SRC_TEENSY): RET_ACK -> captured, RET_FRAME = the Teensy END."""
    rng = random.Random(71)
    fr = []
    for f in range(5):
        lst = []
        for i in range(5):
            cx, cy = rng.uniform(100, 1180), rng.uniform(100, 620)
            add(lst, tri([rand_vtx(rng, cx - 150, cx + 150, cy - 150, cy + 150) for _ in range(3)]))
        add(lst, setup_rect(40 + 200 * f, 600, 140 + 200 * f, 700, 0xFFFF, 0.0, 0)[1])
        fr.append(lst)
    t3, p3 = fr[3][:3], fr[3][3:]
    ps, ts = Stream(), Stream()
    ps.control(4)
    ps.clear(0x0841)
    ps.ret_ack()                             # not full: nothing
    ps.push_recs(fr[0] + [make_end(200)])
    ps.wait(12000)                           # f0 has started rendering (capture decided)
    ps.ret_enable(1)
    ps.wait_swaps(1)
    ps.push_recs(fr[1] + [make_end(201)])    # captured
    ps.wait(4000)
    ps.ret_ack()                             # capture in progress, RET_FULL = 0: nothing
    ps.wait_swaps(2)
    ps.push_recs(fr[2] + [make_end(202)])    # RET_FULL -> not captured
    ps.wait_swaps(3)
    ps.ret_ack()
    ps.ret_enable(0)
    ps.wait(10)
    ps.ret_enable(1)
    ps.control(6)                            # Teensy + PS
    ps.push_recs(p3 + [make_end(203)])       # PS part (read after the Teensy part + END)
    ts.wait_swaps(3)
    ts.wait(40)
    ts.push_recs(t3 + [make_end(903)])       # Teensy part of f3
    ts.wait_swaps(4)
    ps.wait_swaps(4)
    ps.ret_ack()
    ps.control(2)                            # Teensy only
    ts.wait(40)
    ts.push_recs(fr[4] + [make_end(904)])    # f4: frame_no = Teensy END
    write_scene(out, 'ret_capture2', [(f, 0x0841) for f in fr[:3]] + [(t3 + p3, 0x0841), (fr[4], 0x0841)],
                0x0841, streams={'ps': ps, 'teensy': ts}, path='both',
                fnos=[200, 201, 202, 203, 904], ret_caps=[(1, 201), (3, 203), (4, 904)],
                expect={'expect_last_fno': 904})


SCENES = {
    'single_tri': sc_single_tri,
    'rects': sc_rects,
    'zbuf': sc_zbuf,
    'sprite_ckey': sc_sprite,
    'strip_cross': sc_strip_cross,
    'two_frames': sc_two_frames,
    'overflow': sc_overflow,
    'empty': sc_empty,
    'soft_reset': sc_soft_reset,
    'ret_capture': sc_ret_capture,
    'src_switch': sc_src_switch,
    'edge_records': sc_edge_records,
    'bad_records': sc_bad_records,
    'random_raw': sc_random_raw,
    'random_mix': sc_random_mix,
    'random_mix_t': lambda out: sc_random_mix(out, 'random_mix_t', 29, 200, 'teensy'),
    'perf_mesh': sc_perf_mesh,
    'ring_full': sc_ring_full,
    'ring_srst': sc_ring_srst,
    'zclear_frames': sc_zclear_frames,
    'ret_capture2': sc_ret_capture2,
}


def cmd_render(scene_dir):
    """Cross-check another generator's scene: rec.hex -> list -> render -> compare."""
    words = []
    with open(os.path.join(scene_dir, 'rec.hex')) as f:
        for j, line in enumerate(l for l in f if l.strip()):
            words.append((j % REC_WORDS == 0, int(line.strip(), 16)))
    cfg = {}
    with open(os.path.join(scene_dir, 'cfg.txt')) as f:
        for line in f:
            if '=' in line:
                k, v = line.strip().split('=', 1)
                cfg[k] = v
    cc = int(cfg.get('clear_color', '0'), 16)
    lst, bad, ovf, ok = collect(words)
    ddr_path = os.path.join(scene_dir, 'ddr.hex')
    ddr = DDR.load(ddr_path) if os.path.exists(ddr_path) else DDR()
    fb = render(lst, cc, ddr)
    exp = []
    with open(os.path.join(scene_dir, 'expected.hex')) as f:
        for line in f:
            v = int(line.strip(), 16)
            exp.extend([v & 0xFFFF, (v >> 16) & 0xFFFF, (v >> 32) & 0xFFFF, (v >> 48) & 0xFFFF])
    mism = sum(1 for a, b in zip(fb, exp) if a != b)
    print('golden render of %s: list=%d bad=%d overflow=%d end=%s mismatches vs expected.hex=%d'
          % (scene_dir, len(lst), bad, ovf, ok, mism))
    return 0 if mism == 0 and len(exp) == W * H else 1


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    if len(argv) < 2 or argv[1] not in ('gen', 'render', 'list'):
        print(__doc__)
        return 2
    if argv[1] == 'list':
        print('\n'.join(SCENES))
        return 0
    if argv[1] == 'render':
        return cmd_render(argv[2])
    out = argv[2] if len(argv) > 2 else os.path.join(here, 'py_scenes')
    names = argv[3:] or list(SCENES)
    for n in names:
        SCENES[n](out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
