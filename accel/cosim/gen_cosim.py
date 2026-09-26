#!/usr/bin/env python3
"""gen_cosim.py -- memory image and expected results for tb_cosim.v: jobs laid out in a 1 MB
stand-in for the Zynq's DDR3 exactly as zaccel-server lays them out (SPEC §1/§4), results computed
independently from the SPEC.

Writes mem.hex (64-bit words, @word-address blocks), jobs.hex (8 x 32-bit fields per job),
exp.hex (expected output words), expmask.hex, counts.vh.
"""
import random

rng = random.Random(20260925)
MEMW = 1 << 17                     # 1 MB in 64-bit words
mem = {}                           # word address -> value
jobs, exp, mask = [], [], []
cursor = 0x1000                    # byte address allocator


def alloc(nbytes):
    global cursor
    a = cursor
    cursor = (cursor + nbytes + 63) & ~63
    assert cursor < MEMW * 8
    return a


def put_words(addr, words):
    for i, w in enumerate(words):
        mem[addr // 8 + i] = w


def beats_of_bytes(bs):
    out = []
    for j in range(0, len(bs), 8):
        c = bs[j:j + 8] + [0] * (8 - len(bs[j:j + 8]))
        out.append(sum(b << (8 * i) for i, b in enumerate(c)))
    return out


def job(mode, nb, K, N, chunks=1, abort=False):
    lo, hi = (-8, 7) if mode == 0 else (-128, 127)
    A = [[rng.randint(-128, 127) for _ in range(K)] for _ in range(nb)]
    W = [[rng.randint(lo, hi) for _ in range(K)] for _ in range(N)]
    # staging input: header + nb activation vectors (server's pl_gemv)
    abeats = (K + 7) // 8
    hdr = 0x5A41 | (mode << 16) | ((nb - 1) << 20) | (K << 32) | (N << 48)
    inw = [hdr]
    for v in range(nb):
        inw += beats_of_bytes([a & 0xFF for a in A[v]])
    in_addr = alloc(len(inw) * 8); put_words(in_addr, inw)
    # tensor rows, beat-padded (LOAD)
    ww = []
    for r in W:
        if mode == 0:
            nib = [w & 15 for w in r] + [0] * (-K % 16)
            ww += [sum(n << (4 * i) for i, n in enumerate(nib[b:b + 16])) for b in range(0, len(nib), 16)]
        else:
            ww += beats_of_bytes([w & 0xFF for w in r] + [0] * (-K % 8))
    w_addr = alloc(len(ww) * 8); put_words(w_addr, ww)
    nres = N * nb
    out_bytes = (nres + 1) // 2 * 8 + 8
    out_addr = alloc(out_bytes)
    put_words(out_addr, [0xDEADBEEFDEADBEEF] * (out_bytes // 8))   # garbage the engine must overwrite
    w_bytes = len(ww) * 8
    chunk = w_bytes if chunks == 1 else (((w_bytes // chunks) + 7) & ~7)
    vals = [sum(W[r][k] * A[v][k] for k in range(K)) & 0xFFFFFFFF for r in range(N) for v in range(nb)]
    if len(vals) % 2:
        vals.append(0)
    jobs.append([in_addr, len(inw) * 8, w_addr, w_bytes, out_addr, out_bytes, chunk, (1 if abort else 0) | (len(exp) << 8)])
    if not abort:
        for i in range(0, len(vals), 2):
            exp.append(vals[i] | (vals[i + 1] << 32)); mask.append((1 << 64) - 1)
        exp.append((0x5A45 << 48) | (N << 32)); mask.append(0xFFFFFFFF00000000)


job(0, 8, 300, 40)                   # int4, full batch, odd K
job(1, 3, 17, 7)                     # int8, 21 results: padded
job(0, 1, 4096, 24, chunks=3)        # widest row, weights in 3 MM2S transfers (the server's chunking)
job(1, 8, 1000, 30, chunks=2, abort=True)   # started, weights cut short, then the server soft reset
job(0, 5, 129, 33)                   # must be exact after the abort: the reset cleared the engine
job(1, 2, 4096, 9, chunks=2)

with open("mem.hex", "w") as f:
    last = None
    for a in sorted(mem):
        if a != (last or -2) + 1:
            f.write(f"@{a:x}\n")
        f.write(f"{mem[a]:016x}\n"); last = a
with open("jobs.hex", "w") as f:
    for j in jobs:
        f.writelines(f"{v & 0xFFFFFFFF:08x}\n" for v in j)
with open("exp.hex", "w") as f:
    f.writelines(f"{v:016x}\n" for v in exp)
with open("expmask.hex", "w") as f:
    f.writelines(f"{v:016x}\n" for v in mask)
import os
here = os.path.dirname(os.path.abspath(__file__)).replace("\\", "/")
with open("counts.vh", "w") as f:
    f.write(f"`define NJOBS {len(jobs)}\n`define NEXP {len(exp)}\n`define MEMW {MEMW}\n")
    for n in ("mem", "jobs", "exp", "expmask"):          # absolute: xsim runs in its own directory
        f.write(f'`define {n.upper()}_HEX "{here}/{n}.hex"\n')
print(f"{len(jobs)} jobs, {len(exp)} expected words, memory used {cursor} bytes")
