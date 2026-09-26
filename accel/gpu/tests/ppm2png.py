#!/usr/bin/env python3
"""ppm2png.py -- convert a binary PPM (P6, maxval 255) or ASCII PPM (P3) to PNG.

Standard library only (zlib + struct).  Usage: ppm2png.py in.ppm [out.png]
(default output name: the input with .png instead of .ppm)
"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    tokens = []
    pos = 0
    # header: magic, width, height, maxval -- whitespace separated, '#' comments allowed
    while len(tokens) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise ValueError("truncated PPM header")
        tokens.append(data[start:pos])
    magic = tokens[0]
    width, height, maxval = int(tokens[1]), int(tokens[2]), int(tokens[3])
    if width <= 0 or height <= 0 or not 0 < maxval < 256:
        raise ValueError("unsupported PPM geometry/maxval")
    n = width * height * 3
    if magic == b"P6":
        pos += 1  # exactly one whitespace byte after maxval
        pix = data[pos:pos + n]
        if len(pix) != n:
            raise ValueError("truncated PPM pixel data")
    elif magic == b"P3":
        vals = data[pos:].split()
        if len(vals) < n:
            raise ValueError("truncated PPM pixel data")
        pix = bytes(int(v) for v in vals[:n])
    else:
        raise ValueError("not a P6/P3 PPM file")
    if maxval != 255:
        pix = bytes(min(255, (v * 255 + maxval // 2) // maxval) for v in pix)
    return width, height, pix


def png_chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def write_png(path, width, height, pix):
    stride = width * 3
    raw = b"".join(b"\x00" + pix[y * stride:(y + 1) * stride] for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n" +
           png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           png_chunk(b"IDAT", zlib.compress(raw, 9)) +
           png_chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main(argv):
    if len(argv) not in (2, 3):
        sys.stderr.write("usage: ppm2png.py in.ppm [out.png]\n")
        return 1
    src = argv[1]
    dst = argv[2] if len(argv) == 3 else (src[:-4] if src.lower().endswith(".ppm") else src) + ".png"
    width, height, pix = read_ppm(src)
    write_png(dst, width, height, pix)
    print("%s -> %s (%dx%d)" % (src, dst, width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
