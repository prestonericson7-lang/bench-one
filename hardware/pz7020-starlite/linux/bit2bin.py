#!/usr/bin/env python3
"""bit2bin.py -- strip the Xilinx .bit header so the Linux fpga_manager (and U-Boot 'fpga load') can use it.

    python3 bit2bin.py design.bit design.bin

A .bit file = a small tagged header (design name, part, date, time, length) followed by the raw
configuration stream, which begins with 0xFFFFFFFF padding and the 0xAA995566 sync word. The Zynq
fpga_manager wants exactly that raw stream (it also byte-swaps as needed). The header is parsed,
not guessed: field 'e' carries the stream length.
"""
import struct, sys

def strip(bit: bytes) -> tuple[bytes, dict]:
    p = 0
    (n,) = struct.unpack(">H", bit[p:p + 2]); p += 2 + n           # 13-byte magic
    (n,) = struct.unpack(">H", bit[p:p + 2]); p += 2               # 0x0001: the next byte is the first key ('a')
    info = {}
    while p < len(bit):
        key = chr(bit[p]); p += 1
        if key == "e":
            (length,) = struct.unpack(">I", bit[p:p + 4]); p += 4
            data = bit[p:p + length]
            if len(data) != length:
                raise SystemExit(f"truncated: header says {length} bytes, file has {len(data)}")
            if data[:4] != b"\xff\xff\xff\xff" or b"\xaa\x99\x55\x66" not in data[:64]:
                raise SystemExit("payload does not start with 0xFFFFFFFF padding and the AA995566 sync word")
            return data, info
        (n,) = struct.unpack(">H", bit[p:p + 2]); p += 2
        info[key] = bit[p:p + n].rstrip(b"\0").decode("ascii", "replace"); p += n
    raise SystemExit("no 'e' (data) field in header")

if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    data, info = strip(open(src, "rb").read())
    open(dst, "wb").write(data)
    print(f"{src}: {info} -> {dst} ({len(data)} bytes)")
