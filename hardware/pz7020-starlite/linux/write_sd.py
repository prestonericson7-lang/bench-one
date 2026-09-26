"""
write_sd.py -- write the PZ7020-StarLite SD image to a microSD card, unbuffered, and prove it.

Run elevated (raw disk access). Typical:
  Start-Process python -Verb RunAs -ArgumentList '"D:\\espicpc\\hardware\\pz7020-starlite\\linux\\write_sd.py"'
Defaults: image = out/pz7020-starlite-sd.img.xz, expected hash = out/sd-image.sha256,
log = %TEMP%\\pz7020_write_sd.log (last line 'DONE rc=0' on success).

What it guarantees
  * Target is FOUND by facts, never a fixed disk number: exactly one disk that is USB, 28-34 GiB, and an
    SD-reader name ('Generic MassStorageClass' | 'Mass Storage Device'). Zero or several -> refuse.
    Change --min-gib/--max-gib for a different card size.
  * The image hash is checked against the recorded build before anything is written.
  * Written with FILE_FLAG_NO_BUFFERING|WRITE_THROUGH (no cache games: see the throughput line).
  * The partition table is written LAST. Windows re-scans a disk the moment a valid MBR appears and
    invalidates an open raw handle -- measured 2026-09-24 as WinError 433 at 516 MiB when written in order.
  * The whole range is read back unbuffered and hashed; the run fails unless it equals the image hash.
After a successful write Windows may pop "You need to format the disk ... Format disk?" for the ext4
partition -- answer NO.
"""
import argparse, ctypes, ctypes.wintypes as wt, hashlib, lzma, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
OUTD = os.path.join(HERE, "out")
ap = argparse.ArgumentParser()
ap.add_argument("--image", default=os.path.join(OUTD, "pz7020-starlite-sd.img.xz"))
ap.add_argument("--sha256", default=None, help="expected sha256 of the RAW image (default: sd-image.sha256)")
ap.add_argument("--log", default=os.path.join(tempfile.gettempdir(), "pz7020_write_sd.log"))
ap.add_argument("--min-gib", type=float, default=28.0)
ap.add_argument("--max-gib", type=float, default=34.0)
ap.add_argument("--expect", action="append", default=[], metavar="OFFSET=HEX",
                help="bytes the card must ALREADY hold before anything is written (proof it is the right card), "
                     "e.g. the Orange Pi card: 8196=65474f4e2e425430 (eGON.BT0)")
A = ap.parse_args()
if A.sha256 is None:
    A.sha256 = open(os.path.join(OUTD, "sd-image.sha256")).read().split()[0]
CHUNK = 4 << 20

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
k32.WriteFile.restype = wt.BOOL
k32.WriteFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.ReadFile.restype = wt.BOOL
k32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.SetFilePointerEx.restype = wt.BOOL
k32.SetFilePointerEx.argtypes = [wt.HANDLE, ctypes.c_longlong, ctypes.POINTER(ctypes.c_longlong), wt.DWORD]
k32.VirtualAlloc.restype = wt.LPVOID
k32.VirtualAlloc.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.FlushFileBuffers.argtypes = [wt.HANDLE]
k32.CloseHandle.argtypes = [wt.HANDLE]
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
SHARE_RW, OPEN_EXISTING = 3, 3
NO_BUFFERING, WRITE_THROUGH = 0x20000000, 0x80000000
INVALID = wt.HANDLE(-1).value


def log(m):
    with open(A.log, "a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S") + "  " + m + "\n")


def done(rc, msg=None):
    if msg:
        log(("FAIL: " if rc else "") + msg)
    log(f"DONE rc={rc}")
    sys.exit(rc)


def ps(cmd):
    r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd], capture_output=True, text=True)
    return r.returncode, (r.stdout or "").strip(), (r.stderr or "").strip()


def find_card():
    rc, out, err = ps(
        "$c=@(Get-Disk | Where-Object { \"$($_.BusType)\" -eq 'USB' "
        f"-and $_.Size -ge {A.min_gib}GB -and $_.Size -le {A.max_gib}GB "
        "-and $_.FriendlyName -match 'Generic MassStorageClass|Mass Storage Device' }); "
        "if ($c.Count -ne 1) { 'COUNT|' + $c.Count } else { "
        "'{0}|{1}|{2}|{3}|{4}' -f $c[0].Number,$c[0].FriendlyName,$c[0].BusType,$c[0].Size,$c[0].PartitionStyle }")
    if rc or not out or out.startswith("COUNT|"):
        return None, (out or err)
    num, name, bus, size, style = out.split("|")
    return (int(num), name, bus, int(size), style), None


def open_disk(num, access, flags, tries=1):
    for _ in range(tries):
        h = k32.CreateFileW(r"\\.\PhysicalDrive%d" % num, access, SHARE_RW, None, OPEN_EXISTING, flags, None)
        if h is not None and h != INVALID:
            return h
        time.sleep(1)
    done(1, f"CreateFile PhysicalDrive{num} failed, winerr {ctypes.get_last_error()}")


def seek(h, off):
    if not k32.SetFilePointerEx(h, off, None, 0):
        done(1, f"SetFilePointerEx({off}) failed, winerr {ctypes.get_last_error()}")


def check_expect(num):
    """Every --expect OFFSET=HEX must match what the card holds now (read unbuffered, sector-aligned)."""
    if not A.expect:
        return
    tmp = k32.VirtualAlloc(None, 8192, 0x3000, 0x04)
    if not tmp:
        done(1, "VirtualAlloc failed")
    hd = open_disk(num, GENERIC_READ, NO_BUFFERING)
    r = wt.DWORD(0)
    try:
        for e in A.expect:
            off_s, hexs = e.split("=", 1)
            off, want = int(off_s, 0), bytes.fromhex(hexs)
            base = off & ~4095
            n = 8192 if off + len(want) - base > 4096 else 4096
            seek(hd, base)
            if not k32.ReadFile(hd, tmp, n, ctypes.byref(r), None) or r.value != n:
                done(1, f"identity read at {base} failed, winerr {ctypes.get_last_error()}")
            got = ctypes.string_at(tmp + (off - base), len(want))
            log(f"identity: bytes at {off} = {got.hex()} (expected {want.hex()})")
            if got != want:
                done(1, "this card does not hold the expected bytes -- not the card to overwrite; nothing written")
    finally:
        k32.CloseHandle(hd)


def raw_image():
    """Return the path of the raw image, decompressing an .xz and checking its hash on the way."""
    if not A.image.lower().endswith(".xz"):
        return A.image
    dst = os.path.join(tempfile.gettempdir(), os.path.basename(A.image)[:-3])
    log(f"decompressing {A.image} -> {dst}")
    h = hashlib.sha256()
    with lzma.open(A.image, "rb") as f, open(dst, "wb") as o:
        for b in iter(lambda: f.read(CHUNK), b""):
            h.update(b)
            o.write(b)
    if h.hexdigest() != A.sha256:
        done(1, f"decompressed image hash {h.hexdigest()} != expected {A.sha256}")
    return dst


def main():
    if not ctypes.windll.shell32.IsUserAnAdmin():
        done(1, "not elevated -- run with Start-Process ... -Verb RunAs")
    log("elevated: True")
    card, why = find_card()
    if not card:
        done(1, f"no unique SD card found (USB, {A.min_gib}-{A.max_gib} GiB, SD-reader name): {why}")
    num, name, bus, size, style = card
    log(f"target: PhysicalDrive{num} '{name}' bus={bus} size={size / 2**30:.2f} GiB style={style}")
    check_expect(num)

    img = raw_image()
    isz = os.path.getsize(img)
    log(f"image: {img}  {isz} bytes ({isz / 2**20:.0f} MiB)")
    if isz % 4096 or isz <= CHUNK or isz > size:
        done(1, "image not 4 KiB aligned, not larger than one chunk, or larger than the card")
    h = hashlib.sha256()
    with open(img, "rb") as f:
        for b in iter(lambda: f.read(CHUNK), b""):
            h.update(b)
    img_sha = h.hexdigest()
    log(f"image sha256: {img_sha}")
    if img_sha != A.sha256:
        done(1, "image hash does not match the recorded build")

    # an earlier failed "wsl --mount" leaves the disk offline (OfflineReason Policy); Clear-Disk refuses
    # an offline disk. Only reached after the identity checks above, so this is the right card.
    rc, out, err = ps(f"$d = Get-Disk -Number {num}; if ($d.IsOffline) {{ Set-Disk -Number {num} -IsOffline $false; 'brought online' }}")
    if rc:
        done(1, f"Set-Disk -IsOffline $false: {err or out}")
    if out:
        log(f"disk was offline: {out}")
    rc, out, err = ps(f"Clear-Disk -Number {num} -RemoveData -RemoveOEM -Confirm:$false")
    if rc:
        done(1, f"Clear-Disk: {err or out}")
    log("partition table cleared")
    time.sleep(2)

    buf = k32.VirtualAlloc(None, CHUNK, 0x3000, 0x04)
    if not buf:
        done(1, "VirtualAlloc failed")
    w = wt.DWORD(0)

    def put(hd, data, where):
        ctypes.memmove(buf, data, len(data))
        if not k32.WriteFile(hd, buf, len(data), ctypes.byref(w), None) or w.value != len(data):
            done(1, f"WriteFile failed at offset {where}, winerr {ctypes.get_last_error()}")

    log("writing unbuffered (NO_BUFFERING|WRITE_THROUGH): 4 MiB -> end first, partition table last ...")
    hd = open_disk(num, GENERIC_READ | GENERIC_WRITE, NO_BUFFERING | WRITE_THROUGH)
    t0, total = time.time(), 0
    with open(img, "rb") as f:
        f.seek(CHUNK)
        seek(hd, CHUNK)
        off = CHUNK
        for data in iter(lambda: f.read(CHUNK), b""):
            put(hd, data, off)
            off += len(data)
            total += len(data)
            if (off // CHUNK) % 64 == 0:
                log(f"  {off / 2**20:.0f} / {isz / 2**20:.0f} MiB")
        f.seek(0)
        seek(hd, 0)
        first = f.read(CHUNK)
        put(hd, first, 0)
        total += len(first)
    k32.FlushFileBuffers(hd)
    k32.CloseHandle(hd)
    ws = time.time() - t0
    log(f"write complete: {total} bytes in {ws:.1f} s = {total / ws / 1e6:.1f} MB/s "
        "(a real SD card lands in 10-90 MB/s; hundreds would mean cache)")

    time.sleep(5)
    card2, why = find_card()
    if not card2 or card2[0] != num:
        done(1, f"card identity changed after write ({why or card2}) -- read-back not trusted")
    log("reading back unbuffered ...")
    hr = open_disk(num, GENERIC_READ, NO_BUFFERING, tries=20)
    t0, left, rsha, r = time.time(), total, hashlib.sha256(), wt.DWORD(0)
    while left:
        want = min(CHUNK, left)
        if not k32.ReadFile(hr, buf, want, ctypes.byref(r), None) or r.value != want:
            done(1, f"ReadFile failed with {left} bytes left, winerr {ctypes.get_last_error()}")
        rsha.update(ctypes.string_at(buf, want))
        left -= want
    k32.CloseHandle(hr)
    rs = time.time() - t0
    log(f"read-back: {total} bytes in {rs:.1f} s = {total / rs / 1e6:.1f} MB/s")
    log(f"card  sha256 (first {total} bytes): {rsha.hexdigest()}")
    log(f"image sha256:                       {img_sha}")
    if rsha.hexdigest() != img_sha:
        done(1, "READ-BACK MISMATCH -- the card does not hold the image")
    log("READ-BACK MATCH: the card holds the image byte-for-byte")

    ps("Update-HostStorageCache")
    time.sleep(3)
    rc, out, err = ps(
        f"Get-Partition -DiskNumber {num} | ForEach-Object {{ "
        f"'  partition {{0}}: offset={{1}} MiB size={{2}} MiB mbrtype={{3}} letter={{4}}' -f "
        f"$_.PartitionNumber,($_.Offset/1MB),($_.Size/1MB),$_.MbrType,$_.DriveLetter }}")
    for line in (out or err).splitlines():
        log(line)
    done(0)


try:
    main()
except SystemExit:
    raise
except Exception as e:
    done(1, f"exception: {e!r}")
