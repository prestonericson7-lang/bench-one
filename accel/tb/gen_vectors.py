#!/usr/bin/env python3
"""gen_vectors.py -- stimulus and expected output for tb_zaccel_gemv.v, computed straight from
accel/SPEC.md §1 (not from the RTL).

Writes into the current directory:
  in.hex       one 64-bit input beat per line
  exp.hex      one 64-bit expected output beat per line
  expmask.hex  per output beat, which bits are checked (the trailer's cycle count is not)
  explast.hex  per output beat, the expected TLAST (0/1)
  counts.vh    `define NIN / NOUT
"""
import random
import sys

rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
M64 = (1 << 64) - 1
beats_in, exp, mask, last = [], [], [], []


def s8(v):
    return v & 0xFF


def pack_bytes(bs):
    """list of byte values -> 64-bit beats, byte i of beat j = element 8j+i"""
    out = []
    for j in range(0, len(bs), 8):
        chunk = bs[j:j + 8] + [0] * (8 - len(bs[j:j + 8]))
        out.append(sum(b << (8 * i) for i, b in enumerate(chunk)))
    return out


def garbage(n):
    for _ in range(n):
        v = rng.getrandbits(64)
        if v & 0xFFFF == 0x5A41:
            v ^= 1
        beats_in.append(v)


def job(mode, nb, K, N, wmax=None, dirty=False, extreme=False):
    lo, hi = (-8, 7) if mode == 0 else (-128, 127)
    A = [[(-128 if extreme else rng.randint(-128, 127)) for _ in range(K)] for _ in range(nb)]
    W = [[(lo if extreme else rng.randint(lo, hi)) for _ in range(K)] for _ in range(N)]
    hdr = 0x5A41 | (mode << 16) | ((nb - 1) << 20) | (K << 32) | (N << 48)
    beats_in.append(hdr)
    for v in range(nb):
        act = [s8(x) for x in A[v]]
        bts = pack_bytes(act)
        if dirty:                           # garbage in the tail bytes past K
            tail = (8 - K % 8) % 8
            if tail:
                keep = (1 << (8 * (8 - tail))) - 1
                bts[-1] = (bts[-1] & keep) | (rng.getrandbits(64) & ~keep & M64)
        beats_in.extend(bts)
    for r in range(N):
        if mode == 0:
            nib = [(w & 0xF) for w in W[r]]
            if len(nib) % 16:
                pad = 16 - len(nib) % 16
                nib += [rng.getrandbits(4) if dirty else 0 for _ in range(pad)]
            for b in range(0, len(nib), 16):
                beats_in.append(sum(n << (4 * i) for i, n in enumerate(nib[b:b + 16])))
        else:
            byts = [s8(w) for w in W[r]]
            if len(byts) % 8:
                pad = 8 - len(byts) % 8
                byts += [rng.getrandbits(8) if dirty else 0 for _ in range(pad)]
            beats_in.extend(pack_bytes(byts))
    vals = []
    for r in range(N):
        for v in range(nb):
            y = sum(W[r][k] * A[v][k] for k in range(K))
            assert -(1 << 31) <= y < (1 << 31)
            vals.append(y & 0xFFFFFFFF)
    if len(vals) % 2:
        vals.append(0)
    for i in range(0, len(vals), 2):
        exp.append(vals[i] | (vals[i + 1] << 32))
        mask.append(M64)
        last.append(0)
    exp.append((0x5A45 << 48) | ((N & 0xFFFF) << 32))
    mask.append(0xFFFFFFFF00000000)        # cycles (low 32) checked only for nonzero in the TB
    last.append(1)


garbage(3)
job(0, 1, 1, 1)
job(0, 3, 7, 5)
job(0, 8, 16, 2)
garbage(2)
job(0, 8, 17, 300)
job(1, 1, 17, 5)
job(1, 8, 4096, 3)
job(0, 1, 4096, 2)
job(1, 3, 1, 7)                 # 21 values: odd, padded
job(0, 5, 100, 33, dirty=True)  # garbage past K must be masked
job(1, 2, 300, 64, dirty=True)
job(1, 8, 4096, 2, extreme=True)  # -128 * -128 * 4096 on every lane
job(0, 8, 4096, 2, extreme=True)  # -8 * -128 * 4096
garbage(1)
job(0, 2, 33, 40)

with open("in.hex", "w") as f:
    f.writelines(f"{b:016x}\n" for b in beats_in)
with open("exp.hex", "w") as f:
    f.writelines(f"{b:016x}\n" for b in exp)
with open("expmask.hex", "w") as f:
    f.writelines(f"{b:016x}\n" for b in mask)
with open("explast.hex", "w") as f:
    f.writelines(f"{b:x}\n" for b in last)
with open("counts.vh", "w") as f:
    f.write(f"`define NIN {len(beats_in)}\n`define NOUT {len(exp)}\n`define NJOBS {sum(last)}\n")
print(f"{len(beats_in)} input beats, {len(exp)} output beats, {sum(last)} jobs")
