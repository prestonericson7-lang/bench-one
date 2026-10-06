#!/usr/bin/env python3
"""read_cards.py -- READ-ONLY evidence pass over the two cards in the dual-slot reader (run elevated).

Never writes to a card. One card at a time (concurrent jobs on this reader once corrupted a read-back).
Each card is identified by facts before anything is read; a card that does not match is skipped.

  FPGA card #1  -- USB, 28-34 GiB, MBR, p1 FAT32 at 1 MiB / 128 MiB, p2 at 129 MiB: the whole image span
                   (the first 1,746,927,616 bytes, the 2026-09-26 image length) is copied to
                   <out>/fpga1-card.img and hashed. The board booted this card on real hardware
                   (2026-10-05); its rootfs keeps a persistent journal.
  Orange Pi card -- USB, 100-130 GiB, the Allwinner boot signature eGON.BT0 at byte 8196, ext4 at 32 MiB:
                   chosen files and directory listings printed to <out>/pi-files.txt, and where the bytes
                   of /usr/local/sbin/nvme-auto sit on the card (<out>/pi-extents.txt).

  Start-Process python -Verb RunAs -Wait -ArgumentList '"D:\\espicpc\\machine\\cards\\read_cards.py" 3 4'
  args: FPGA_DISK PI_DISK [OUTDIR]   log: <out>/read_cards.log, last line 'DONE rc=0'
"""
import contextlib
import ctypes
import ctypes.wintypes as wt
import hashlib
import io
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "hardware", "pz7020-starlite", "linux")))
import ext4_read  # noqa: E402  (read-only ext4 reader)

FPGA_DISK, PI_DISK = int(sys.argv[1]), int(sys.argv[2])
OUT = sys.argv[3] if len(sys.argv) > 3 else r"D:\start\machine-cards\evidence"
os.makedirs(OUT, exist_ok=True)
LOG = os.path.join(OUT, "read_cards.log")
FPGA_SPAN = 1746927616
CH = 4 << 20

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
k32.ReadFile.restype = wt.BOOL
k32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.SetFilePointerEx.restype = wt.BOOL
k32.SetFilePointerEx.argtypes = [wt.HANDLE, ctypes.c_longlong, ctypes.POINTER(ctypes.c_longlong), wt.DWORD]
k32.VirtualAlloc.restype = wt.LPVOID
k32.VirtualAlloc.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]


def log(m):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S  ") + m + "\n")


def ps(cmd):
    r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd], capture_output=True, text=True)
    return (r.stdout or "").strip()


def disk_facts(n):
    out = ps(f"$d=Get-Disk -Number {n}; '{{0}}|{{1}}|{{2}}|{{3}}' -f $d.BusType,$d.Size,$d.PartitionStyle,$d.FriendlyName; "
             f"Get-Partition -DiskNumber {n} | ForEach-Object {{ 'P|{{0}}|{{1}}|{{2}}' -f $_.PartitionNumber,$_.Offset,$_.Size }}")
    lines = out.splitlines()
    bus, size, style, name = lines[0].split("|")
    parts = [tuple(int(x) for x in l.split("|")[1:]) for l in lines[1:] if l.startswith("P|")]
    return bus, int(size), style, name, parts


def raw_read(n, off, length):
    h = k32.CreateFileW(r"\\.\PhysicalDrive%d" % n, 0x80000000, 3, None, 3, 0x20000000, None)
    if h in (None, wt.HANDLE(-1).value):
        raise OSError(f"open PhysicalDrive{n}: winerr {ctypes.get_last_error()}")
    buf = k32.VirtualAlloc(None, 8192, 0x3000, 0x04)
    a = off & ~4095
    k32.SetFilePointerEx(h, a, None, 0)
    r = wt.DWORD(0)
    k32.ReadFile(h, buf, 8192, ctypes.byref(r), None)
    k32.CloseHandle(h)
    return ctypes.string_at(buf + (off - a), length)


def dump(n, length, path):
    h = k32.CreateFileW(r"\\.\PhysicalDrive%d" % n, 0x80000000, 3, None, 3, 0x20000000, None)
    if h in (None, wt.HANDLE(-1).value):
        raise OSError(f"open PhysicalDrive{n}: winerr {ctypes.get_last_error()}")
    buf = k32.VirtualAlloc(None, CH, 0x3000, 0x04)
    r = wt.DWORD(0)
    sha = hashlib.sha256()
    t0, done = time.time(), 0
    with open(path, "wb") as f:
        while done < length:
            want = min(CH, length - done)
            want = (want + 4095) & ~4095
            if not k32.ReadFile(h, buf, want, ctypes.byref(r), None) or r.value != want:
                k32.CloseHandle(h)
                raise OSError(f"read failed at {done}: winerr {ctypes.get_last_error()}")
            d = ctypes.string_at(buf, min(want, length - done))
            f.write(d)
            sha.update(d)
            done += len(d)
    k32.CloseHandle(h)
    return sha.hexdigest(), done / max(time.time() - t0, 1e-6) / 2**20


def main():
    rc = 0
    log(f"elevated: {bool(ctypes.windll.shell32.IsUserAnAdmin())}; FPGA disk {FPGA_DISK}, Pi disk {PI_DISK}")
    # ---- FPGA card #1 (a negative disk number skips it) ----
    try:
        if FPGA_DISK < 0:
            raise StopIteration
        bus, size, style, name, parts = disk_facts(FPGA_DISK)
        log(f"disk {FPGA_DISK}: {bus} {size / 2**30:.2f} GiB {style} '{name}' parts={parts}")
        p1 = parts[0] if parts else None
        ok = (bus == "USB" and 28 * 2**30 <= size <= 34 * 2**30 and style == "MBR" and p1 and p1[1] == 2**20
              and p1[2] == 128 * 2**20 and raw_read(FPGA_DISK, 2**20 + 82, 8) == b"FAT32   ")
        if not ok:
            log("FPGA card: identity does NOT match -- skipped"); rc = 1
        else:
            sha, rate = dump(FPGA_DISK, FPGA_SPAN, os.path.join(OUT, "fpga1-card.img"))
            log(f"FPGA card: {FPGA_SPAN} bytes copied to fpga1-card.img at {rate:.1f} MiB/s, sha256 {sha}")
    except StopIteration:
        log("FPGA card: skipped (disk < 0)")
    except Exception as e:
        log(f"FPGA card: ERROR {e}"); rc = 1
    # ---- Orange Pi card ----
    try:
        bus, size, style, name, parts = disk_facts(PI_DISK)
        log(f"disk {PI_DISK}: {bus} {size / 2**30:.2f} GiB {style} '{name}' parts={parts}")
        egon = raw_read(PI_DISK, 8196, 8)
        p1 = parts[0] if parts else None
        lba = p1[1] // 512 if p1 else 0
        sb = raw_read(PI_DISK, (p1[1] if p1 else 0) + 1024 + 0x38, 2)
        ok = (bus == "USB" and 100 * 2**30 <= size <= 130 * 2**30 and egon == b"eGON.BT0" and p1 and p1[1] == 32 * 2**20
              and sb == b"\x53\xef")
        log(f"Pi card: eGON at 8196 = {egon!r}, ext4 magic {sb.hex()}, partition LBA {lba}")
        if not ok:
            log("Pi card: identity does NOT match -- skipped"); rc = 1
        else:
            src = r"\\.\PhysicalDrive%d" % PI_DISK
            paths = ["/etc/os-release", "/etc/hostname", "/etc/fstab", "/etc/fstab.nvme-auto.orig",
                     "/var/lib/accel/", "/var/lib/accel/firstboot.log", "/home/orangepi/", "/home/orangepi/accel-install.log",
                     "/usr/local/sbin/nvme-auto", "/etc/systemd/system/nvme-auto.service",
                     "/etc/systemd/system/multi-user.target.wants/", "/var/log/", "/var/log.hdd/", "/var/log.hdd/journal/",
                     "/var/log.hdd/syslog", "/var/log.hdd/kern.log", "/opt/accel/", "/usr/local/bin/",
                     "/etc/NetworkManager/system-connections/"]
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                sys.argv = ["ext4_read.py", src, str(lba)] + paths
                try:
                    ext4_read.main()
                except SystemExit as e:
                    print(f"ext4_read stopped: {e}")
            open(os.path.join(OUT, "pi-files.txt"), "w", encoding="utf-8").write(buf.getvalue())
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                sys.argv = ["ext4_read.py", src, str(lba), "--extents", "/usr/local/sbin/nvme-auto", "/etc/fstab"]
                ext4_read.main()
            open(os.path.join(OUT, "pi-extents.txt"), "w", encoding="utf-8").write(buf.getvalue())
            log(f"Pi card: files and extents written ({os.path.getsize(os.path.join(OUT, 'pi-files.txt'))} bytes)")
    except Exception as e:
        log(f"Pi card: ERROR {e}"); rc = 1
    log(f"DONE rc={rc}")


if __name__ == "__main__":
    main()
