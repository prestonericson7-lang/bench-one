"""ext4_read.py SOURCE PART_LBA PATH [PATH/ ...] -- read-only: print files (or, with a trailing slash,
list directories) from an ext4 partition on a raw
disk (\\\\.\\PhysicalDriveN, run elevated) or in an image file, without mounting it. Windows cannot mount
ext4 and WSL cannot attach a USB SD reader, so this is how the PC reads what a board wrote to its card
(e.g. the Orange Pi's /var/lib/accel/firstboot.log). Opens the source read-only; never writes.

Handles what current mke2fs/resize2fs produce: 64bit group descriptors, flex_bg, extent trees of any
depth, linear and htree directories (dirents are read linearly from every block). No inline data."""
import struct
import sys

SECTOR = 512


class Disk:
    def __init__(self, path, part_lba):
        self.f = open(path, "rb", buffering=0)
        self.base = part_lba * SECTOR

    def read(self, off, n):
        """n bytes at partition offset off; raw disks need sector-aligned reads, so align here"""
        a = (self.base + off) // 4096 * 4096
        e = -(-(self.base + off + n) // 4096) * 4096
        self.f.seek(a)
        d = self.f.read(e - a)
        return d[self.base + off - a: self.base + off - a + n]


class Ext4:
    def __init__(self, disk):
        self.d = disk
        sb = disk.read(1024, 1024)
        if struct.unpack_from("<H", sb, 0x38)[0] != 0xEF53:
            raise SystemExit("no ext4 superblock at this LBA")
        self.bs = 1024 << struct.unpack_from("<I", sb, 0x18)[0]
        self.ipg = struct.unpack_from("<I", sb, 0x28)[0]
        self.isz = struct.unpack_from("<H", sb, 0x58)[0]
        incompat = struct.unpack_from("<I", sb, 0x60)[0]
        self.desc = struct.unpack_from("<H", sb, 0xFE)[0] if incompat & 0x80 else 32
        if incompat & 0x10:
            raise SystemExit("meta_bg is not handled")
        self.gdt = (struct.unpack_from("<I", sb, 0x14)[0] + 1) * self.bs

    def inode(self, ino):
        g, i = divmod(ino - 1, self.ipg)
        gd = self.d.read(self.gdt + g * self.desc, self.desc)
        tbl = struct.unpack_from("<I", gd, 8)[0] | (struct.unpack_from("<I", gd, 0x28)[0] << 32 if self.desc >= 64 else 0)
        return self.d.read(tbl * self.bs + i * self.isz, self.isz)

    def extents(self, node):
        magic, entries, _, depth = struct.unpack_from("<HHHH", node, 0)
        if magic != 0xF30A:
            raise SystemExit("not an extent tree (old block-mapped file?)")
        for k in range(entries):
            e = node[12 + 12 * k: 24 + 12 * k]
            if depth == 0:
                lblk, ln, hi, lo = struct.unpack("<IHHI", e)
                # ln > 32768: allocated but uninitialised -- reads as zeros, whatever the blocks hold
                yield lblk, ((hi << 32) | lo), ln if ln <= 32768 else ln - 32768, ln > 32768   # 32768 = a full initialised extent
            else:
                _, lo, hi, _ = struct.unpack("<IIHH", e)
                yield from self.extents(self.d.read(((hi << 32) | lo) * self.bs, self.bs))

    def data(self, ino):
        n = self.inode(ino)
        size = struct.unpack_from("<I", n, 4)[0] | (struct.unpack_from("<I", n, 0x6C)[0] << 32)
        out = bytearray(size)
        for lblk, pblk, ln, uninit in self.extents(n[0x28:0x28 + 60]):
            chunk = bytes(ln * self.bs) if uninit else self.d.read(pblk * self.bs, ln * self.bs)
            s = lblk * self.bs
            if s < size:
                out[s:min(size, s + len(chunk))] = chunk[:size - s]
        return bytes(out)

    def entries(self, ino):
        """(name, inode) of every live entry in directory ino"""
        d = self.data(ino)
        for b in range(0, len(d), self.bs):
            blk, o = d[b:b + self.bs], 0
            while o + 8 <= len(blk):
                child, rec, nl = struct.unpack_from("<IHB", blk, o)
                if rec < 8:
                    break
                name = blk[o + 8:o + 8 + nl].decode("utf-8", "replace")
                if child and name not in (".", ".."):
                    yield name, child
                o += rec

    def size(self, ino):
        n = self.inode(ino)
        return struct.unpack_from("<I", n, 4)[0] | (struct.unpack_from("<I", n, 0x6C)[0] << 32)

    def symlink(self, ino):
        """the target of symlink ino, or None if ino is not a symlink"""
        n = self.inode(ino)
        if struct.unpack_from("<H", n, 0)[0] & 0xF000 != 0xA000:
            return None
        size = self.size(ino)
        if size < 60 and not struct.unpack_from("<I", n, 0x20)[0] & 0x80000:    # fast symlink: target in i_block
            return n[0x28:0x28 + size].decode("utf-8", "replace")
        return self.data(ino).decode("utf-8", "replace")

    def lookup(self, path, follow_last=False, depth=0):
        """inode of path; symlinks in the middle are followed (the last one only with follow_last)"""
        parts = [p for p in path.split("/") if p]
        ino, walked = 2, []
        for k, name in enumerate(parts):
            ino = next((c for n, c in self.entries(ino) if n == name), None)
            if ino is None:
                return None
            t = self.symlink(ino)
            if t is not None and (k < len(parts) - 1 or follow_last):
                if depth > 16:
                    return None
                base = "" if t.startswith("/") else "/" + "/".join(walked)
                return self.lookup(base + "/" + t + "/" + "/".join(parts[k + 1:]), follow_last, depth + 1)
            walked.append(name)
        return ino


def main():
    sys.stdout.reconfigure(encoding="utf-8")     # logs carry arrows etc.; a redirected console is cp1252
    src, lba, paths = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    fs = Ext4(Disk(src, lba))
    for p in paths:
        ino = fs.lookup(p)
        if ino is None:
            print(f"===== {p}: not found")
            continue
        if p.endswith("/"):                       # a trailing slash lists the directory
            ino = fs.lookup(p, follow_last=True)
            print(f"===== {p} (directory, inode {ino})")
            for n, c in sorted(fs.entries(ino)):
                t = fs.symlink(c)
                print(f"{fs.size(c):>12}  {n}" + (f" -> {t}" if t is not None else ""))
            continue
        t = fs.symlink(ino)
        if t is not None:
            print(f"===== {p} -> {t}")
            ino = fs.lookup(p, follow_last=True)
            if ino is None:
                print("      (target not found)")
                continue
        d = fs.data(ino)
        print(f"===== {p} (inode {ino}, {len(d)} bytes)")
        sys.stdout.write(d.decode("utf-8", "replace"))
        if d and not d.endswith(b"\n"):
            print()


if __name__ == "__main__":
    main()
