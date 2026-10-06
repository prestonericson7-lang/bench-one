#!/usr/bin/env python3
"""jbd2_timeline.py IMG BOOT_EPOCH -- read-only forensics of an ext4 image whose journal was NOT replayed:
superblock times, then every jbd2 transaction (sequence, commit time as uptime, blocks logged), and for
each logged inode-table block the inodes whose copy in the journal differs from the copy in place,
with their timestamps as uptime. Opens the image read-only; writes nothing.

BOOT_EPOCH is the wall-clock time at uptime 0: any journal entry's __REALTIME_TIMESTAMP minus its
__MONOTONIC_TIMESTAMP (journalctl -o export). Commits stop when the board stops writing; the journal's
own last entry only marks its last flush. Used on card #1 after the 2026-10-05 boot (docs/61):
    python3 jbd2_timeline.py /root/fpga1-evidence/p2.img 1777319332.506
Timestamps are whole seconds, so an event printed at N.494 s happened in [N.494, N+1.494)."""
import struct, sys, datetime

IMG, B = sys.argv[1], float(sys.argv[2])
f = open(IMG, "rb")


def rd(off, n):
    f.seek(off); return f.read(n)


def up(t):
    return "%8.3f s" % (t - B) if t >= B - 1e6 else "(%s)" % datetime.datetime.utcfromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S")


sb = rd(1024, 1024)
u32 = lambda b, o: struct.unpack_from("<I", b, o)[0]
u16 = lambda b, o: struct.unpack_from("<H", b, o)[0]
bs = 1024 << u32(sb, 24)
ipg, isz = u32(sb, 40), u16(sb, 88)
first_data = u32(sb, 20)
inc = u32(sb, 96)
is64 = bool(inc & 0x80)
dsz = u16(sb, 254) if is64 else 32
ngroups = (u32(sb, 4) + u32(sb, 32) - 1 - first_data) // u32(sb, 32) + 1
print(f"block size {bs}, inodes/group {ipg}, inode size {isz}, groups {ngroups}, 64bit {is64}")
print(f"superblock: state 0x{u16(sb, 58):x} (1 = clean), mount count {u16(sb, 52)}, needs_recovery {bool(inc & 0x4)}")
print(f"  last mount  s_mtime {up(u32(sb, 44))}   last write s_wtime {up(u32(sb, 48))}   mkfs {up(u32(sb, 264))}")
print(f"  lifetime writes s_kbytes_written {struct.unpack_from('<Q', sb, 376)[0]} KiB")

gdt = rd((first_data + 1) * bs, ngroups * dsz)
itab = []
for g in range(ngroups):
    d = gdt[g * dsz:(g + 1) * dsz]
    lo = u32(d, 8); hi = u32(d, 0x28) if is64 and dsz >= 64 else 0
    itab.append(lo | (hi << 32))
itab_blocks = ipg * isz // bs


def inode_raw(n):
    g, i = divmod(n - 1, ipg)
    return rd(itab[g] * bs + i * isz, isz)


def extents(raw_iblock):
    out = []

    def walk(node):
        magic, ent, mx, depth = struct.unpack_from("<HHHH", node, 0)
        if magic != 0xF30A:
            return
        for k in range(ent):
            e = node[12 + 12 * k: 24 + 12 * k]
            if depth == 0:
                lb, ln, shi, slo = struct.unpack("<IHHI", e)
                if ln > 32768: ln -= 32768
                out.append((lb, (shi << 32) | slo, ln))
            else:
                lb, llo, lhi, _ = struct.unpack("<IIHH", e)
                walk(rd(((lhi << 32) | llo) * bs, bs))
    walk(raw_iblock)
    return out


jino = u32(sb, 224)
jin = inode_raw(jino)
jext = extents(jin[40:100])


def jblock(n):                       # journal-relative block -> image offset
    for lb, pb, ln in jext:
        if lb <= n < lb + ln:
            return (pb + n - lb) * bs
    raise ValueError(n)


be32 = lambda b, o: struct.unpack_from(">I", b, o)[0]
jsb = rd(jblock(0), bs)
assert be32(jsb, 0) == 0xC03B3998, "no jbd2 superblock"
jmax, jfirst, jseq, jstart = be32(jsb, 16), be32(jsb, 20), be32(jsb, 24), be32(jsb, 28)
jinc = be32(jsb, 40)
csum3, csum2, j64 = bool(jinc & 0x10), bool(jinc & 0x8), bool(jinc & 0x2)
tagsz = (16 if j64 else 12) if csum3 else (8 + (4 if j64 else 0)) + (2 if csum2 else 0) - (2 if csum2 else 0)
if not csum3:
    tagsz = 12 if j64 else 8
print(f"\njournal inode {jino}: {jmax} blocks, first {jfirst}, s_sequence {jseq}, s_start {jstart} "
      f"(0 = nothing to replay), incompat 0x{jinc:x} (csum v3 {csum3}, 64bit {j64}), tag size {tagsz}")


def walk_journal():
    txs, pos, seq, cur = [], jstart, jseq, []
    while True:
        b = rd(jblock(pos), bs)
        if be32(b, 0) != 0xC03B3998 or be32(b, 8) != seq:
            break
        t = be32(b, 4)
        nxt = lambda p: jfirst + (p + 1 - jfirst) % (jmax - jfirst)
        if t == 1:                                     # descriptor
            o = 12
            end = bs - (4 if (csum2 or csum3) else 0)
            while o + tagsz <= end:
                blk = be32(b, o)
                flags = be32(b, o + 4) if csum3 else struct.unpack_from(">H", b, o + 6)[0]
                hi = be32(b, o + 8) if j64 else 0
                pos = nxt(pos)
                cur.append(((hi << 32) | blk, pos))
                o += tagsz
                if not flags & 0x2:
                    o += 16
                if flags & 0x8:
                    break
            pos = nxt(pos)
        elif t == 2:                                   # commit
            sec = struct.unpack_from(">Q", b, 48)[0]; nsec = be32(b, 56)
            txs.append((seq, sec + nsec / 1e9, cur)); cur = []
            seq += 1; pos = nxt(pos)
        elif t == 5:                                   # revoke
            pos = nxt(pos)
        else:
            break
    return txs, cur


txs, open_tx = walk_journal()
print(f"{len(txs)} committed transaction(s) not yet checkpointed; uncommitted tail: {len(open_tx)} block(s)")


def ts(raw):
    t = {"atime": u32(raw, 8), "ctime": u32(raw, 12), "mtime": u32(raw, 16)}
    if isz > 128 and u16(raw, 128) >= 24:
        t["crtime"] = u32(raw, 144)
    return t


for seq, ct, blocks in txs:
    kinds = {}
    for fsb, jpos in blocks:
        g = next((g for g in range(ngroups) if itab[g] <= fsb < itab[g] + itab_blocks), None)
        kinds.setdefault("inode table" if g is not None else "other metadata", []).append((fsb, jpos, g))
    print(f"\n== transaction {seq}: committed at uptime {up(ct)}; {len(blocks)} block(s): "
          + ", ".join(f"{k} {len(v)}" for k, v in kinds.items()))
    for fsb, jpos, g in kinds.get("inode table", []):
        new = rd(jblock(jpos), bs); old = rd(fsb * bs, bs)
        base = g * ipg + (fsb - itab[g]) * (bs // isz) + 1
        for k in range(bs // isz):
            a, b2 = old[k * isz:(k + 1) * isz], new[k * isz:(k + 1) * isz]
            if a != b2 and u16(b2, 0):
                links = u16(b2, 26)
                if u16(b2, 0) == 0xFFFF or links == 0 or (u16(b2, 0) & 0xF000) not in (0x4000, 0x8000, 0xA000):
                    print(f"   inode {base + k:7d} (unused slot: uninitialised bytes, mode {u16(b2, 0):06o}, links {links})")
                    continue
                t = ts(b2)
                print(f"   inode {base + k:7d} mode {u16(b2, 0):06o} links {links} size {u32(b2, 4):9d}  "
                      + "  ".join(f"{n} {up(v)}" for n, v in t.items()))
