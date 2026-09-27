"""
verify_cards.py -- read-only proof that both bench SD cards still hold their images (run elevated).
For each card: find it by facts (USB, size range, SD-reader name), check its identity bytes, read the
image's length back unbuffered, and compare the SHA-256 with the recorded image hash. One card at a time
(both sit in one dual-slot reader; concurrent jobs there once corrupted a read-back). Never writes.
Log: %TEMP%\\verify_cards.log, last line 'DONE rc=0' when both match.
"""
import ctypes, ctypes.wintypes as wt, hashlib, os, subprocess, sys, tempfile, time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))


def recorded(rel):
    """the image hash the build recorded (first field of its .sha256 file), so this never goes stale"""
    return open(os.path.join(REPO, rel), encoding="utf-8").read().split()[0].lower()


CARDS = [
    dict(name="Zynq (PZ7020)", lo=28, hi=34, size=1746927616,
         sha=recorded("hardware/pz7020-starlite/linux/out/sd-image.sha256"),
         expect={1048647: "424f4f5420202020202020"}),                      # FAT label "BOOT"
    dict(name="Orange Pi", lo=100, hi=130, size=11211374592,
         sha=recorded("accel/pi-card/opi4pro-accel.img.sha256"),
         expect={8196: "65474f4e2e425430", 33555560: "a81ee6f15c1244f08e92c28029cab13c"}),  # eGON.BT0, rootfs UUID
]
LOG = os.path.join(tempfile.gettempdir(), "verify_cards.log")
CH = 4 << 20
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
k32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.SetFilePointerEx.argtypes = [wt.HANDLE, ctypes.c_longlong, ctypes.POINTER(ctypes.c_longlong), wt.DWORD]
k32.VirtualAlloc.restype = wt.LPVOID
k32.VirtualAlloc.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]
L = open(LOG, "w")


def log(m):
    L.write(time.strftime("%H:%M:%S  ") + m + "\n"); L.flush()


def find(c):
    ps = ("$d=@(Get-Disk | Where-Object { \"$($_.BusType)\" -eq 'USB' -and "
          f"$_.Size -ge {c['lo']}GB -and $_.Size -le {c['hi']}GB -and "
          "$_.FriendlyName -match 'Generic MassStorageClass|Mass Storage Device' }); "
          "if ($d.Count -eq 1) { $d[0].Number } else { 'COUNT ' + $d.Count }")
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                         capture_output=True, text=True).stdout.strip()
    return int(out) if out.isdigit() else None


def main():
    if not ctypes.windll.shell32.IsUserAnAdmin():
        log("not elevated"); log("DONE rc=1"); return 1
    buf = k32.VirtualAlloc(None, CH, 0x3000, 0x04)
    r = wt.DWORD(0)
    rc = 0
    for c in CARDS:
        n = find(c)
        if n is None:
            log(f"{c['name']}: card not found (USB, {c['lo']}-{c['hi']} GiB)"); rc = 1; continue
        h = k32.CreateFileW(r"\\.\PhysicalDrive%d" % n, 0x80000000, 3, None, 3, 0x20000000, None)
        ok = True
        for off, hx in c["expect"].items():
            base = off & ~4095
            k32.SetFilePointerEx(h, base, None, 0)
            k32.ReadFile(h, buf, 8192, ctypes.byref(r), None)
            got = ctypes.string_at(buf + (off - base), len(hx) // 2).hex()
            ok &= got == hx
        if not ok:
            log(f"{c['name']}: PhysicalDrive{n} does not carry its identity bytes"); rc = 1
            k32.CloseHandle(h); continue
        k32.SetFilePointerEx(h, 0, None, 0)
        sha, left, t0 = hashlib.sha256(), c["size"], time.time()
        while left:
            want = min(CH, left)
            if not k32.ReadFile(h, buf, want, ctypes.byref(r), None) or r.value != want:
                log(f"{c['name']}: read failed, {left} bytes left"); rc = 1; break
            sha.update(ctypes.string_at(buf, want)); left -= want
        k32.CloseHandle(h)
        if left == 0:
            got = sha.hexdigest()
            same = got == c["sha"]
            log(f"{c['name']}: PhysicalDrive{n}, {c['size']} bytes in {time.time() - t0:.0f} s, sha256 {got[:16]}... "
                + ("MATCHES the image" if same else f"DIFFERS from the image {c['sha'][:16]}..."))
            if not same:
                # A whole-card hash also changes when Windows mounts a FAT partition and adds its
                # "System Volume Information" (Zynq card, 2026-09-26: 312 bytes of FAT bookkeeping + one
                # directory, boot files identical), or when the Pi has booted the card (first boot writes
                # the rootfs). card_compare.py DISK IMAGE LOG says which bytes differ.
                log(f"{c['name']}: find out where with card_compare.py {n} <image> <log> before rewriting anything")
            rc |= 0 if same else 1
    log(f"DONE rc={rc}")
    return rc


sys.exit(main())
