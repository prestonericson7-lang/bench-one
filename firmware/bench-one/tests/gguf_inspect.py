#!/usr/bin/env python3
"""
gguf_inspect.py -- read a real model file and report what it would take to run it here

WHY THIS EXISTS
---------------
Everything measured so far is infrastructure: how fast a board reads memory, what a nibble multiply
costs, what a hop between nodes costs. None of it has touched an actual model. This reads one.

GGUF is llama.cpp's format and it carries everything needed in a single file: the architecture
config, the full tokenizer, and every tensor with its shape, quantization and byte offset. Ollama
stores its models as GGUF blobs, so there are already real models on this machine.

WHAT IT ANSWERS
---------------
* Which layers exist, how big each is, and therefore how they split across nodes.
* Which quantization each tensor uses, since Q4_0 and Q4_K are laid out differently and the loader
  has to handle whichever is actually present rather than whichever was assumed.
* How large the KV cache grows, which on a 13 MB node is often larger than the weights.
* Whether the model fits the hardware at all, and if so how it should be partitioned.

The point is to replace assumptions with the file. A planner fed guessed layer sizes produces a
guessed plan.
"""

import json
import os
import struct
import sys

# GGUF metadata value types
T_UINT8, T_INT8, T_UINT16, T_INT16, T_UINT32, T_INT32 = 0, 1, 2, 3, 4, 5
T_FLOAT32, T_BOOL, T_STRING, T_ARRAY, T_UINT64, T_INT64, T_FLOAT64 = 6, 7, 8, 9, 10, 11, 12

# ggml tensor types that matter here: (name, block size in elements, bytes per block)
GGML = {
    0:  ("F32",    1,  4),
    1:  ("F16",    1,  2),
    2:  ("Q4_0",  32, 18),
    3:  ("Q4_1",  32, 20),
    6:  ("Q5_0",  32, 22),
    7:  ("Q5_1",  32, 24),
    8:  ("Q8_0",  32, 34),
    9:  ("Q8_1",  32, 36),
    10: ("Q2_K", 256, 84),
    11: ("Q3_K", 256, 110),
    12: ("Q4_K", 256, 144),
    13: ("Q5_K", 256, 176),
    14: ("Q6_K", 256, 210),
    15: ("Q8_K", 256, 292),
}


class Reader:
    def __init__(self, f):
        self.f = f

    def u32(self):   return struct.unpack("<I", self.f.read(4))[0]
    def u64(self):   return struct.unpack("<Q", self.f.read(8))[0]
    def i32(self):   return struct.unpack("<i", self.f.read(4))[0]
    def f32(self):   return struct.unpack("<f", self.f.read(4))[0]

    def string(self):
        n = self.u64()
        return self.f.read(n).decode("utf-8", "replace")

    def value(self, t):
        if t == T_UINT8:   return struct.unpack("<B", self.f.read(1))[0]
        if t == T_INT8:    return struct.unpack("<b", self.f.read(1))[0]
        if t == T_UINT16:  return struct.unpack("<H", self.f.read(2))[0]
        if t == T_INT16:   return struct.unpack("<h", self.f.read(2))[0]
        if t == T_UINT32:  return self.u32()
        if t == T_INT32:   return self.i32()
        if t == T_FLOAT32: return self.f32()
        if t == T_BOOL:    return struct.unpack("<?", self.f.read(1))[0]
        if t == T_STRING:  return self.string()
        if t == T_UINT64:  return self.u64()
        if t == T_INT64:   return struct.unpack("<q", self.f.read(8))[0]
        if t == T_FLOAT64: return struct.unpack("<d", self.f.read(8))[0]
        if t == T_ARRAY:
            et = self.u32()
            n = self.u64()
            # A tokenizer's vocabulary is an array of 150,000 strings. Reading it into memory to
            # print a count is pointless, so long arrays are summarised rather than materialised.
            if n > 64:
                for _ in range(n):
                    self.value(et)
                return "<%d items of type %d>" % (n, et)
            return [self.value(et) for _ in range(n)]
        raise ValueError("unknown metadata type %d" % t)


def read_gguf(path):
    f = open(path, "rb")
    r = Reader(f)
    magic = f.read(4)
    if magic != b"GGUF":
        raise ValueError("not a GGUF file (magic %r)" % magic)
    version = r.u32()
    n_tensors = r.u64()
    n_kv = r.u64()

    meta = {}
    for _ in range(n_kv):
        k = r.string()
        t = r.u32()
        meta[k] = r.value(t)

    tensors = []
    for _ in range(n_tensors):
        name = r.string()
        nd = r.u32()
        dims = [r.u64() for _ in range(nd)]
        ttype = r.u32()
        off = r.u64()
        tensors.append({"name": name, "dims": dims, "type": ttype, "offset": off})

    align = meta.get("general.alignment", 32)
    data_start = f.tell()
    if data_start % align:
        data_start += align - (data_start % align)
    f.close()
    return version, meta, tensors, data_start


def tensor_bytes(t):
    n = 1
    for d in t["dims"]:
        n *= d
    name, blk, bpb = GGML.get(t["type"], ("T%d" % t["type"], 1, 4))
    return (n // blk) * bpb if blk > 1 else n * bpb


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    version, meta, tensors, data_start = read_gguf(path)

    arch = meta.get("general.architecture", "?")
    g = lambda k, d=0: meta.get("%s.%s" % (arch, k), d)

    print()
    print("=" * 74)
    print("  %s" % os.path.basename(path))
    print("  %.2f GB on disk, GGUF v%d, %d tensors" %
          (os.path.getsize(path) / 1e9, version, len(tensors)))
    print("=" * 74)
    print()
    print("  architecture   %s" % arch)
    print("  name           %s" % meta.get("general.name", "?"))
    print("  layers         %s" % g("block_count"))
    print("  dim            %s" % g("embedding_length"))
    print("  hidden         %s" % g("feed_forward_length"))
    print("  heads          %s   kv heads %s" % (g("attention.head_count"), g("attention.head_count_kv")))
    print("  context        %s" % g("context_length"))
    print("  vocab          %s" % len(meta.get("tokenizer.ggml.tokens", [])) or meta.get("tokenizer.ggml.tokens"))

    # quantization mix -- the loader has to handle what is actually here
    mix = {}
    for t in tensors:
        nm = GGML.get(t["type"], ("T%d" % t["type"],))[0]
        e = mix.setdefault(nm, [0, 0])
        e[0] += 1
        e[1] += tensor_bytes(t)
    print()
    print("  quantization actually used:")
    for nm, (cnt, by) in sorted(mix.items(), key=lambda kv: -kv[1][1]):
        print("    %-6s %4d tensors  %8.1f MB  %5.1f%%" %
              (nm, cnt, by / 1e6, 100.0 * by / sum(v[1] for v in mix.values())))

    # per-layer size, which is what decides the partitioning
    per_layer = {}
    other = 0
    for t in tensors:
        parts = t["name"].split(".")
        if len(parts) > 2 and parts[0] == "blk" and parts[1].isdigit():
            per_layer.setdefault(int(parts[1]), 0)
            per_layer[int(parts[1])] += tensor_bytes(t)
        else:
            other += tensor_bytes(t)
    if per_layer:
        sizes = sorted(per_layer.values())
        print()
        print("  %d repeating layers, %.1f MB each" % (len(per_layer), sizes[len(sizes)//2] / 1e6))
        print("  embeddings and head outside the layers: %.1f MB" % (other / 1e6))

    # the KV cache, which grows and is often the thing that does not fit
    nkv, hd, ctx = g("attention.head_count_kv", 0), 0, g("context_length", 0)
    if g("attention.head_count", 0):
        hd = g("embedding_length", 0) // g("attention.head_count", 1)
    if nkv and hd and ctx:
        per_l_full = 2 * nkv * hd * ctx
        print()
        print("  KV cache at INT8, per layer:")
        for c in (512, 2048, ctx):
            print("    %6d tokens  %8.2f MB" % (c, 2 * nkv * hd * c / 1e6))
        print("    at full context that is %.1f MB a layer, against %.1f MB of weights"
              % (per_l_full / 1e6, sizes[len(sizes)//2] / 1e6 if per_layer else 0))

    print()
    print("  tensor data starts at byte %d" % data_start)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
