#!/usr/bin/env python3
"""esp_image_same.py A.merged.bin B.merged.bin -- is B the same firmware as A?

An ESP32 app image carries three fields that hash the build rather than being the build:
  * esp_app_desc_t.app_elf_sha256 -- a SHA-256 of the whole ELF, debug sections included, so it
    changes with the build directory while no loaded byte does;
  * the one-byte XOR checksum after the last segment (it covers the descriptor, so it follows);
  * the SHA-256 digest appended after the checksum (it covers everything before it).
And one loaded field that is a clock, not the source: the ESP32 Arduino core's chip-debug report
compiles __TIME__ and __DATE__ into "Software Info: Compile Date/Time", so a clean build of the core
differs from a cached one in those characters and nowhere else (measured 2026-09-25: 07:35:53 vs
21:04:10, 6 bytes). A difference is allowed there only when BOTH images hold a well-formed
"HH:MM:SS" / "Mmm dd yyyy" string at the very same offset.
The images are the same firmware when every other byte is identical AND each image's own checksum
and digest are correct for its own bytes (the bootloader checks exactly those two). The app image is
parsed, not guessed at: header -> segments -> padding -> checksum -> digest.
Exit 0 = same firmware; 1 = a real difference (printed).
"""
import hashlib
import re
import struct
import sys

STAMPS = (re.compile(rb"\d\d:\d\d:\d\d\x00"), re.compile(rb"[A-Z][a-z]{2} [ \d]\d \d{4}\x00"))


def stamp_bytes(a, b, lo, hi):
    """Offsets inside a build-time string that both images hold at the same place."""
    out = set()
    for pat in STAMPS:
        for m in pat.finditer(a, lo, hi):
            if pat.fullmatch(b, m.start(), m.end()):
                out.update(range(m.start(), m.end()))
    return out

APP = 0x10000                  # app0 in the default partition table, as merged by arduino-cli
DESC = APP + 0x20              # first segment's data: the app descriptor
ELF_SHA = range(DESC + 0x90, DESC + 0xB0)


def parse(d, name):
    """-> (checksum offset, digest offset) after checking both are right for this image's bytes."""
    if d[APP] != 0xE9:
        sys.exit(f"{name}: no app image at 0x{APP:x}")
    if struct.unpack_from("<I", d, DESC)[0] != 0xABCD5432:
        sys.exit(f"{name}: no app descriptor at 0x{DESC:x}")
    off, csum = APP + 24, 0xEF
    for _ in range(d[APP + 1]):
        _, ln = struct.unpack_from("<II", d, off)
        off += 8
        for b in d[off:off + ln]:
            csum ^= b
        off += ln
    off += 15 - (off - APP) % 16              # esptool pads so the checksum ends a 16-byte block
    if d[off] != csum:
        print(f"{name}: checksum byte 0x{d[off]:02x} != 0x{csum:02x} computed -- the image is corrupt")
        return None
    if not d[APP + 23]:
        return off, None
    if hashlib.sha256(d[APP:off + 1]).digest() != d[off + 1:off + 33]:
        print(f"{name}: appended SHA-256 does not match the image -- the image is corrupt")
        return None
    return off, off + 1


def main():
    a, b = (open(p, "rb").read() for p in sys.argv[1:3])
    if len(a) != len(b):
        print(f"sizes differ: {len(a)} vs {len(b)}")
        return 1
    pa, pb = parse(a, sys.argv[1]), parse(b, sys.argv[2])
    if not pa or not pb:
        return 1
    if pa != pb:
        print(f"image layouts differ: checksum at 0x{pa[0]:x} vs 0x{pb[0]:x}")
        return 1
    skip = set(ELF_SHA) | {pa[0]} | (set(range(pa[1], pa[1] + 32)) if pa[1] else set())
    rest = [i for i in range(len(a)) if a[i] != b[i] and i not in skip]
    stamps = stamp_bytes(a, b, APP, pa[0]) if rest else set()
    clock = [i for i in rest if i in stamps]
    rest = [i for i in rest if i not in stamps]
    if rest:
        print(f"REAL DIFFERENCE: {len(rest)} bytes outside the build hashes, 0x{rest[0]:x}..0x{rest[-1]:x}")
        return 1
    if a == b:
        print("identical")
    else:
        print("same firmware: both images verify; they differ only in the ELF hash, their own "
              "checksum/digest" + (f" and {len(clock)} bytes of the core's build time/date" if clock else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
