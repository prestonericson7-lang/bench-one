"""card_compare.py DISKNUM IMAGE LOG -- read-only: where does the card differ from the image?
Reads \\\\.\\PhysicalDriveN unbuffered in 4 MiB chunks alongside the image; logs every differing chunk
with its first/last differing byte and the count, plus a hex peek at the first difference."""
import ctypes, ctypes.wintypes as wt, sys, time
num, img, logp = int(sys.argv[1]), sys.argv[2], sys.argv[3]
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
k32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.VirtualAlloc.restype = wt.LPVOID
k32.VirtualAlloc.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
CH = 4 << 20
L = open(logp, "w")
def log(m):
    L.write(time.strftime("%H:%M:%S  ") + m + "\n"); L.flush()
h = k32.CreateFileW(r"\\.\PhysicalDrive%d" % num, 0x80000000, 3, None, 3, 0x20000000, None)
if h in (None, wt.HANDLE(-1).value):
    log(f"open failed {ctypes.get_last_error()}"); log("DONE rc=1"); sys.exit(1)
buf = k32.VirtualAlloc(None, CH, 0x3000, 0x04)
r = wt.DWORD(0)
bad = 0
off = 0
with open(img, "rb") as f:
    while True:
        a = f.read(CH)
        if not a:
            break
        if not k32.ReadFile(h, buf, len(a), ctypes.byref(r), None) or r.value != len(a):
            log(f"read failed at {off} err {ctypes.get_last_error()}"); break
        b = ctypes.string_at(buf, len(a))
        if a != b:
            d = [i for i in range(len(a)) if a[i] != b[i]]
            bad += 1
            if bad <= 40:
                i = d[0]
                log(f"chunk @ {off:#x} ({off >> 20} MiB): {len(d)} bytes differ, first {off + i:#x} last {off + d[-1]:#x}; "
                    f"image {a[i:i+16].hex()} card {b[i:i+16].hex()}")
        off += len(a)
log(f"compared {off} bytes; {bad} chunks differ")
log("DONE rc=0")
