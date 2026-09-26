#!/usr/bin/env python3
"""cmp_fb.py -- compare a rendered frame dump against the expected frame.

Usage: cmp_fb.py actual.hex expected.hex [png_prefix] [--max N]
  Files: 230400 lines x 16 hex digits (64-bit AXI beats, pixel 4k in bits [15:0]).
  Prints the mismatch count and the first N mismatches as "x,y expected actual".
  If png_prefix is given (or there are mismatches), writes <prefix>_expected.png,
  <prefix>_actual.png and <prefix>_diff.png (diff: mismatching pixels red, others dimmed).
  Exit status: 0 = identical, 1 = mismatch, 2 = bad input.
"""
import struct
import sys
import zlib

W, H = 1280, 720


def load(path):
    px = []
    with open(path) as f:
        for n, line in enumerate(f):
            line = line.strip()
            if not line:
                continue
            try:
                v = int(line, 16)
            except ValueError:
                raise ValueError('%s:%d: not hex: %r' % (path, n + 1, line))
            px.extend((v & 0xFFFF, (v >> 16) & 0xFFFF, (v >> 32) & 0xFFFF, (v >> 48) & 0xFFFF))
    if len(px) != W * H:
        raise ValueError('%s: %d pixels, expected %d' % (path, len(px), W * H))
    return px


def rgb888(p):
    r5, g6, b5 = (p >> 11) & 31, (p >> 5) & 63, p & 31
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def write_png(path, rows):
    """rows: list of H bytearrays of W*3 RGB bytes."""
    raw = b''.join(b'\x00' + bytes(r) for r in rows)

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def image_rows(px):
    rows = []
    for y in range(H):
        r = bytearray()
        for x in range(W):
            r.extend(rgb888(px[y * W + x]))
        rows.append(r)
    return rows


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    maxn = 20
    for i, a in enumerate(argv):
        if a == '--max' and i + 1 < len(argv):
            maxn = int(argv[i + 1])
            if argv[i + 1] in args:
                args.remove(argv[i + 1])
    if len(args) < 2:
        print(__doc__)
        return 2
    try:
        act = load(args[0])
        exp = load(args[1])
    except (OSError, ValueError) as e:
        print('cmp_fb: ERROR: %s' % e)
        return 2
    prefix = args[2] if len(args) > 2 else None
    bad = [i for i in range(W * H) if act[i] != exp[i]]
    print('cmp_fb: %s vs %s: %d mismatching pixels' % (args[0], args[1], len(bad)))
    for i in bad[:maxn]:
        print('  x=%d y=%d expected=%04x actual=%04x' % (i % W, i // W, exp[i], act[i]))
    if bad:
        ys = sorted(set(i // W for i in bad))
        print('  rows affected: %d (first %d, last %d); strips: %s' %
              (len(ys), ys[0], ys[-1], sorted(set(y // 16 for y in ys))[:20]))
    if prefix is None and bad:
        prefix = args[0].rsplit('.', 1)[0]
    if prefix:
        write_png(prefix + '_expected.png', image_rows(exp))
        write_png(prefix + '_actual.png', image_rows(act))
        badset = set(bad)
        rows = []
        for y in range(H):
            r = bytearray()
            for x in range(W):
                i = y * W + x
                if i in badset:
                    r.extend((255, 0, 0))
                else:
                    c = rgb888(exp[i])
                    r.extend((c[0] // 4, c[1] // 4, c[2] // 4))
            rows.append(r)
        write_png(prefix + '_diff.png', rows)
        print('  wrote %s_{expected,actual,diff}.png' % prefix)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
