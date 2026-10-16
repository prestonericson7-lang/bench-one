#!/usr/bin/env python3
"""copy_model_to_card.py -- put a model file on the Teensy's microSD card (in this PC's reader) and PROVE it arrived.

    python copy_model_to_card.py <source.gguf> <drive:\\name.gguf> [--sha256 HEX]

1. Copies in 8 MB pieces, then fsync (FlushFileBuffers): Windows' write cache holds the last of it otherwise.
2. Reads the copy BACK FROM THE CARD with FILE_FLAG_NO_BUFFERING -- a plain read right after a copy is served from
   the PC's RAM, not the card (the raw-disk trap of 2026-09-24: 951 MB/s was the tell) -- and hashes it.
3. Compares with --sha256, or with the source's name when it is an Ollama blob (sha256-<hex>: the name IS the hash),
   or with a hash of the source.
Prints the rates, so a cached read cannot pass for a card read (a UHS-I card reads under ~95 MB/s)."""
import argparse
import ctypes
import hashlib
import os
import re
import sys
import time
from ctypes import wintypes

PIECE = 8 << 20


def read_unbuffered(path, size):
    """SHA-256 of the file read with FILE_FLAG_NO_BUFFERING: sector-aligned reads into a page-aligned buffer"""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                wintypes.DWORD, wintypes.HANDLE]
    k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    k32.VirtualAlloc.restype = wintypes.LPVOID
    k32.VirtualAlloc.argtypes = [wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
    GENERIC_READ, OPEN_EXISTING, FILE_SHARE_READ = 0x80000000, 3, 1
    NO_BUFFERING, SEQUENTIAL = 0x20000000, 0x08000000
    h = k32.CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, None, OPEN_EXISTING, NO_BUFFERING | SEQUENTIAL, None)
    if h == wintypes.HANDLE(-1).value or not h:
        raise OSError("CreateFile %s: error %d" % (path, ctypes.get_last_error()))
    buf = k32.VirtualAlloc(None, PIECE, 0x3000, 0x04)               # MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE: page-aligned
    if not buf:
        raise OSError("VirtualAlloc: error %d" % ctypes.get_last_error())
    hsh, done, t0 = hashlib.sha256(), 0, time.time()
    got = wintypes.DWORD(0)
    while done < size:
        if not k32.ReadFile(h, buf, PIECE, ctypes.byref(got), None):
            raise OSError("ReadFile at %d: error %d" % (done, ctypes.get_last_error()))
        if got.value == 0:
            break
        n = min(got.value, size - done)                             # the last sector-rounded read runs past the end
        hsh.update(ctypes.string_at(buf, n))
        done += n
        if done % (1 << 30) < PIECE:
            print("  read back %.1f of %.1f GB, %.1f MB/s" % (done / 1e9, size / 1e9, done / 1e6 / (time.time() - t0)), flush=True)
    k32.CloseHandle(h)
    return hsh.hexdigest(), done, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--sha256")
    a = ap.parse_args()
    size = os.path.getsize(a.src)
    want = a.sha256
    if not want:
        m = re.search(r"sha256-([0-9a-f]{64})$", a.src)
        want = m.group(1) if m else None
    free = ctypes.c_ulonglong(0)
    ctypes.windll.kernel32.GetDiskFreeSpaceExW(os.path.splitdrive(os.path.abspath(a.dst))[0] + "\\", None, None, ctypes.byref(free))
    print("source %s: %d bytes; card free %d bytes" % (a.src, size, free.value), flush=True)
    if free.value < size:
        sys.exit("the card does not have room")
    t0, hs = time.time(), hashlib.sha256()
    with open(a.src, "rb") as fi, open(a.dst, "wb") as fo:
        fo.truncate(size)                                           # one allocation up front: the card can lay it out in one run
        fo.seek(0)
        done = 0
        while True:
            b = fi.read(PIECE)
            if not b:
                break
            if not want:
                hs.update(b)
            fo.write(b)
            done += len(b)
            if done % (1 << 30) < PIECE:
                print("  written %.1f of %.1f GB, %.1f MB/s" % (done / 1e9, size / 1e9, done / 1e6 / (time.time() - t0)), flush=True)
        fo.flush()
        os.fsync(fo.fileno())
    tw = time.time() - t0
    want = want or hs.hexdigest()
    print("written %d bytes in %.0f s (%.1f MB/s), flushed to the card" % (done, tw, done / 1e6 / tw), flush=True)
    got, n, tr = read_unbuffered(os.path.abspath(a.dst), size)
    print("read back from the card: %d bytes in %.0f s (%.1f MB/s)" % (n, tr, n / 1e6 / tr))
    ok = got == want and n == size
    print("sha256 %s %s %s" % (got, "==" if ok else "!=", want))
    print("COPY VERIFIED" if ok else "COPY BAD")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
