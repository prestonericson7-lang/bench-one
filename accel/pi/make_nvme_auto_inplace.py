#!/usr/bin/env python3
"""make_nvme_auto_inplace.py -- accel/pi/nvme-auto as a file of EXACTLY the size the Orange Pi card's copy
has (7959 bytes, the 2026-09-26 version), so it can be written over that file's own data blocks on the card
without touching any ext4 metadata (size, extents and checksums all stay as they are; the card's root
filesystem is data=writeback, so file data never passes through its journal). Comment lines are dropped
to make room; the result is padded with one trailing comment line to the exact size. Nothing is changed in
the script itself -- in particular no claim is baked in: the in-place file never erases anything.

    python accel/pi/make_nvme_auto_inplace.py        -> accel/pi/out/nvme-auto.inplace + its sha256
"""
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "nvme-auto")
OUT = os.path.join(HERE, "out", "nvme-auto.inplace")
SIZE = 7959                                   # the card's /usr/local/sbin/nvme-auto, inode 204672

src = open(SRC, "rb").read().decode("utf-8").replace("\r\n", "\n")
lines = src.split("\n")
out = [lines[0]]                              # the shebang
for ln in lines[1:]:
    if not ln.strip() or re.match(r"^\s*#", ln):
        continue
    out.append(ln)
body = ("\n".join(out) + "\n").encode("utf-8")
note = b"# nvme-auto, written in place on this card 2026-10-05 (source: accel/pi/nvme-auto) "
room = SIZE - len(body) - len(note) - 1
if room < 0:
    sys.exit(f"too big: {len(body) + len(note) + 1} bytes > {SIZE}")
data = body + note + b"#" * room + b"\n"
assert len(data) == SIZE
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "wb").write(data)
print(f"{OUT}: {len(data)} bytes (script {len(body)}, padding {room}), sha256 {hashlib.sha256(data).hexdigest()}")
