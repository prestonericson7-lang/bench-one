#!/usr/bin/env python3
"""
moe_shape.py -- what a mixture-of-experts model is actually made of, read from the file

WHY THIS EXISTS
---------------
Every figure in the 30B plan so far came from an architecture card: 128 experts, 8 firing, expert
intermediate 768, head_dim 128. The file on this disk says head_dim 64 and hidden 5472, so at least
one of those was wrong, and a plan built on the wrong expert size is wrong by whatever factor.

This reads the tensor table and reports, per layer:

  * which tensors are per-expert and which are shared
  * how many bytes one expert costs
  * how many bytes have to be resident no matter what (attention, routers, norms)
  * how many bytes a single token actually touches

The last two are the whole argument for running a 30B on this machine. A dense model touches every
weight every token, which is why streaming one off an SSD is hopeless. A mixture-of-experts touches
a small, data-dependent slice, and the question becomes a cache-hit question instead of a bandwidth
one.

RUN
    python moe_shape.py <model.gguf>
"""

import struct
import sys

GGUF_MAGIC = 0x46554747

# ggml type -> (block size in elements, bytes per block)
TYPES = {
    0: ("F32",   1,  4), 1: ("F16",   1,  2),
    2: ("Q4_0", 32, 18), 3: ("Q4_1", 32, 20),
    6: ("Q5_0", 32, 22), 7: ("Q5_1", 32, 24),
    8: ("Q8_0", 32, 34), 9: ("Q8_1", 32, 40),
    10: ("Q2_K", 256,  84), 11: ("Q3_K", 256, 110),
    12: ("Q4_K", 256, 144), 13: ("Q5_K", 256, 176),
    14: ("Q6_K", 256, 210), 15: ("Q8_K", 256, 292),
}


class Reader:
    def __init__(self, f):
        self.f = f

    def u32(self): return struct.unpack("<I", self.f.read(4))[0]
    def u64(self): return struct.unpack("<Q", self.f.read(8))[0]
    def i32(self): return struct.unpack("<i", self.f.read(4))[0]
    def f32(self): return struct.unpack("<f", self.f.read(4))[0]

    def string(self):
        n = self.u64()
        return self.f.read(n).decode("utf-8", "replace")

    def value(self, t):
        # 0 u8 1 i8 2 u16 3 i16 4 u32 5 i32 6 f32 7 bool 8 string 9 array 10 u64 11 i64 12 f64
        if t == 0:  return struct.unpack("<B", self.f.read(1))[0]
        if t == 1:  return struct.unpack("<b", self.f.read(1))[0]
        if t == 2:  return struct.unpack("<H", self.f.read(2))[0]
        if t == 3:  return struct.unpack("<h", self.f.read(2))[0]
        if t == 4:  return self.u32()
        if t == 5:  return self.i32()
        if t == 6:  return self.f32()
        if t == 7:  return struct.unpack("<?", self.f.read(1))[0]
        if t == 8:  return self.string()
        if t == 9:
            et = self.u32()
            n = self.u64()
            # Arrays here are token lists and merge lists; only the length matters.
            if et == 8:
                for _ in range(n):
                    self.f.read(self.u64())
                return "<%d strings>" % n
            return [self.value(et) for _ in range(n)]
        if t == 10: return self.u64()
        if t == 11: return struct.unpack("<q", self.f.read(8))[0]
        if t == 12: return struct.unpack("<d", self.f.read(8))[0]
        raise ValueError("unknown metadata type %d" % t)


def read(path):
    with open(path, "rb") as f:
        r = Reader(f)
        if r.u32() != GGUF_MAGIC:
            sys.exit("not a GGUF file")
        r.u32()                       # version
        ntensor = r.u64()
        nmeta = r.u64()
        meta = {}
        for _ in range(nmeta):
            k = r.string()
            meta[k] = r.value(r.u32())
        tensors = []
        for _ in range(ntensor):
            name = r.string()
            nd = r.u32()
            dims = [r.u64() for _ in range(nd)]
            t = r.u32()
            off = r.u64()
            tname, blk, bb = TYPES.get(t, ("T%d" % t, 1, 4))
            elems = 1
            for d in dims:
                elems *= d
            tensors.append((name, dims, tname, elems * bb // blk))
        return meta, tensors


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: moe_shape.py <model.gguf>")
    meta, tensors = read(sys.argv[1])

    arch = meta.get("general.architecture", "?")
    def g(key, default=None):
        return meta.get("%s.%s" % (arch, key), default)

    L = g("block_count", 0)
    nexp = g("expert_count", 0)
    nused = g("expert_used_count", 0)
    eff = g("expert_feed_forward_length", 0)

    print("\n==========================================================================")
    print("  %s" % meta.get("general.name", "?"))
    print("==========================================================================")
    print("  architecture %s, %d layers, dim %d" % (arch, L, g("embedding_length", 0)))
    print("  %d experts, %d fire per token, expert intermediate %d"
          % (nexp, nused, eff))
    print("  %d heads / %d kv heads, head_dim %d"
          % (g("attention.head_count", 0), g("attention.head_count_kv", 0),
             g("attention.key_length", 0) or
             (g("embedding_length", 0) // max(1, g("attention.head_count", 1)))))

    # Layer 0 is representative; the tensor table repeats it 48 times.
    l0 = [t for t in tensors if t[0].startswith("blk.0.")]
    print("\n  every tensor in one layer:")
    per_expert_total = 0
    shared_total = 0
    for name, dims, tname, nbytes in sorted(l0, key=lambda t: -t[3]):
        is_exp = "exps" in name
        print("    %-34s %-6s %-22s %9.2f MB%s"
              % (name, tname, "x".join(str(d) for d in dims), nbytes / 1048576.0,
                 "   <- one per expert" if is_exp else ""))
        if is_exp:
            per_expert_total += nbytes
        else:
            shared_total += nbytes

    outside = [t for t in tensors if not t[0].startswith("blk.")]
    outside_bytes = sum(t[3] for t in outside)
    print("\n  outside the layers:")
    for name, dims, tname, nbytes in sorted(outside, key=lambda t: -t[3]):
        print("    %-34s %-6s %-22s %9.2f MB"
              % (name, tname, "x".join(str(d) for d in dims), nbytes / 1048576.0))

    one_expert = per_expert_total / max(1, nexp)
    print("\n  --------------------------------------------------------------------")
    print("  per layer, shared and resident        %9.2f MB" % (shared_total / 1048576.0))
    print("  per layer, all %3d experts           %9.2f MB" % (nexp, per_expert_total / 1048576.0))
    print("  one expert                            %9.2f MB" % (one_expert / 1048576.0))
    print("  outside the layers                    %9.2f MB" % (outside_bytes / 1048576.0))

    resident = shared_total * L + outside_bytes
    touched = (shared_total + one_expert * nused) * L + outside_bytes
    total = (shared_total + per_expert_total) * L + outside_bytes
    print("\n  whole model                           %9.2f GB" % (total / 1073741824.0))
    print("  must be resident (no experts)         %9.2f GB" % (resident / 1073741824.0))
    print("  touched by ONE token                  %9.2f GB   (%.1f%% of the model)"
          % (touched / 1073741824.0, 100.0 * touched / total))
    print("  of which experts                      %9.2f MB"
          % (one_expert * nused * L / 1048576.0))

    print("\n  This machine holds 3.10 GB.")
    spare = 3.10 * 1073741824.0 - resident
    if spare > 0:
        print("  After the resident part there is %.2f GB left, which caches %d experts of %d (%.0f%%)."
              % (spare / 1073741824.0, int(spare / one_expert), nexp * L,
                 100.0 * (spare / one_expert) / (nexp * L)))
    else:
        print("  The resident part alone does not fit: %.2f GB short."
              % (-spare / 1073741824.0))
    print()


if __name__ == "__main__":
    main()
