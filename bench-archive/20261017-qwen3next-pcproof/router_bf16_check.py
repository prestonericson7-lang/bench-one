"""Are the GGUF's F32 tensors exactly bfloat16 values (low 16 bits of every float zero)? Reads the local file's
tensor table, then every F32 tensor's bytes, and counts floats whose low half is nonzero."""
import struct, sys, collections
import numpy as np

path = sys.argv[1]
f = open(path, "rb")
def u32(): return struct.unpack("<I", f.read(4))[0]
def u64(): return struct.unpack("<Q", f.read(8))[0]
def s(): return f.read(u64()).decode("utf-8", "replace")
SZ = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}
assert f.read(4) == b"GGUF"
u32(); nt = u64(); nkv = u64()
align = 32
for _ in range(nkv):
    k = s(); t = u32()
    if t == 8: s()
    elif t == 9:
        at = u32(); n = u64()
        if at == 8:
            for _ in range(n): s()
        else: f.seek(SZ[at] * n, 1)
    else:
        v = f.read(SZ[t])
        if k == "general.alignment": align = struct.unpack("<I", v)[0]
tens = []
for _ in range(nt):
    name = s(); nd = u32(); dims = [u64() for _ in range(nd)]; typ = u32(); off = u64()
    tens.append((name, dims, typ, off))
here = f.tell()
ds = here + (align - here % align) % align
fam = collections.OrderedDict()
for name, dims, typ, off in tens:
    if typ != 0: continue
    n = 1
    for d in dims: n *= d
    f.seek(ds + off)
    a = np.frombuffer(f.read(n * 4), dtype="<u4")
    low = int(np.count_nonzero(a & 0xFFFF))
    key = name.split(".", 2)[-1] if name.startswith("blk.") else name
    st = fam.setdefault(key, [0, 0, 0])
    st[0] += 1; st[1] += n; st[2] += low
for k, (cnt, n, low) in fam.items():
    print("%-28s x%-3d %12d floats, %12d with a nonzero low half%s" % (k, cnt, n, low, "  -> EXACTLY bfloat16" if low == 0 else ""))
