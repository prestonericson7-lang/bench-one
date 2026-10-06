#!/usr/bin/env python3
"""write_cards.py -- the two writes for the cards now in the dual-slot reader (run elevated), one at a time.

  1. FPGA card #1 (disk FPGA_DISK): the machine image as staged by stage.py (the .xz and its raw sha256),
     through hardware/pz7020-starlite/linux/write_sd.py (decompressed hash checked before writing,
     unbuffered, partition table last, whole read-back hashed against the build). write_sd.py is told to
     REFUSE unless the card still holds card #1's root-filesystem UUID 873bd848-2fa9-4d26-8790-bba09025037c
     at byte 135267432 -- the card whose first hardware boot was copied to evidence/fpga1-card.img first.
  2. Orange Pi card (disk PI_DISK): ONE file's data rewritten in place -- /usr/local/sbin/nvme-auto, inode
     204672, 7959 bytes in 2 blocks at byte 8781074432 -- with accel/pi/out/nvme-auto.inplace (same size,
     built by accel/pi/make_nvme_auto_inplace.py, sha256 bae89d00...; those exact bytes are what
     accel/pi/test_nvme_pi_drive.sh runs, with this host's tools and the Pi's own). The new file never erases anything: it adds an ext4 partition in
     the drive's unpartitioned space. No ext4 metadata is touched (size, extents, checksums unchanged; the
     card's root filesystem is data=writeback, so file data does not go through the journal). Refuses
     unless: the card carries eGON.BT0 at 8196 and its partition at 32 MiB, and the 7959 bytes on the card
     hash to the known old file (20884297...). Windows' RAW volume on that partition is locked and
     dismounted for the write; the blocks are read back and compared.

  Start-Process python -Verb RunAs -Wait -ArgumentList '"D:\\espicpc\\machine\\cards\\write_cards.py" 3 4'
  log: D:\\start\\machine-cards\\evidence\\write_cards.log, last line 'DONE rc=0'
"""
import ctypes
import ctypes.wintypes as wt
import hashlib
import os
import subprocess
import sys
import time

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EVID = r"D:\start\machine-cards\evidence"
LOG = os.path.join(EVID, "write_cards.log")
FPGA_DISK, PI_DISK = int(sys.argv[1]), int(sys.argv[2])
IMG = r"D:\start\machine-cards\zynq\pz7020-starlite-sd.img.xz"      # staged by stage.py
SHAF = r"D:\start\machine-cards\zynq\sd-image.sha256"               # the raw image's sha256, staged with it
CARD1_UUID_OFF, CARD1_UUID = 135267432, "873bd8482fa94d268790bba09025037c"
PI_PART_OFF = 32 * 2**20
FILE_OFF, FILE_SIZE, FILE_BLOCKS = 8781074432, 7959, 2
OLD_SHA = "20884297215c3182b11966bb0a9db61f0a37203dbc9aa43a7a422b08c29afc49"
NEW_SHA = "bae89d0040dbf795d5828d73740f46fddb392276dc4198d8bf29f2158e9352a2"   # the bytes the drive test ran
NEW = os.path.join(REPO, "accel", "pi", "out", "nvme-auto.inplace")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
k32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.WriteFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.SetFilePointerEx.argtypes = [wt.HANDLE, ctypes.c_longlong, ctypes.POINTER(ctypes.c_longlong), wt.DWORD]
k32.DeviceIoControl.argtypes = [wt.HANDLE, wt.DWORD, wt.LPVOID, wt.DWORD, wt.LPVOID, wt.DWORD,
                                ctypes.POINTER(wt.DWORD), wt.LPVOID]
k32.VirtualAlloc.restype = wt.LPVOID
k32.VirtualAlloc.argtypes = [wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.FlushFileBuffers.argtypes = [wt.HANDLE]
k32.CloseHandle.argtypes = [wt.HANDLE]
GR, GW, SHARE, OPEN, NOBUF, WTHRU = 0x80000000, 0x40000000, 3, 3, 0x20000000, 0x80000000
FSCTL_LOCK_VOLUME, FSCTL_DISMOUNT_VOLUME = 0x00090018, 0x00090020
BAD = wt.HANDLE(-1).value


def log(m):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S  ") + m + "\n")


def ps(cmd):
    r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd], capture_output=True, text=True)
    return (r.stdout or "").strip()


def opn(path, access, flags=NOBUF):
    h = k32.CreateFileW(path, access, SHARE, None, OPEN, flags, None)
    if h in (None, BAD):
        raise OSError(f"open {path}: winerr {ctypes.get_last_error()}")
    return h


def pread(h, off, n, buf):
    k32.SetFilePointerEx(h, off, None, 0)
    r = wt.DWORD(0)
    if not k32.ReadFile(h, buf, n, ctypes.byref(r), None) or r.value != n:
        raise OSError(f"read {n} at {off}: winerr {ctypes.get_last_error()}")
    return ctypes.string_at(buf, n)


def fpga_card():
    sha = open(SHAF).read().split()[0]
    log(f"FPGA card #1: writing {IMG} (raw sha256 {sha}) with write_sd.py, guarded by the card #1 UUID")
    wlog = os.path.join(EVID, "write_fpga1.log")
    if os.path.exists(wlog):
        os.remove(wlog)
    rc = subprocess.run([sys.executable, os.path.join(REPO, "hardware", "pz7020-starlite", "linux", "write_sd.py"),
                         "--image", IMG, "--sha256", sha, "--log", wlog,
                         "--expect", f"{CARD1_UUID_OFF}={CARD1_UUID}"]).returncode
    for line in open(wlog, encoding="utf-8").read().splitlines():
        log("  write_sd: " + line[10:] if len(line) > 10 else line)
    if rc:
        raise RuntimeError(f"write_sd.py rc={rc}")
    if f"PhysicalDrive{FPGA_DISK} " not in open(wlog, encoding="utf-8").read():
        raise RuntimeError(f"write_sd.py wrote a disk other than {FPGA_DISK} -- check the log")


def pi_card():
    new = open(NEW, "rb").read()
    if len(new) != FILE_SIZE:
        raise RuntimeError(f"{NEW} is {len(new)} bytes, not {FILE_SIZE}")
    if hashlib.sha256(new).hexdigest() != NEW_SHA:
        raise RuntimeError(f"{NEW} is not the tested file (sha256 {hashlib.sha256(new).hexdigest()})")
    buf = k32.VirtualAlloc(None, 1 << 16, 0x3000, 0x04)
    hd = opn(r"\\.\PhysicalDrive%d" % PI_DISK, GR)
    try:
        egon = pread(hd, 8192, 4096, buf)[4:12]
        part = int(ps(f"(Get-Partition -DiskNumber {PI_DISK} -PartitionNumber 1).Offset") or 0)
        old = pread(hd, FILE_OFF, FILE_BLOCKS * 4096, buf)
    finally:
        k32.CloseHandle(hd)
    log(f"Pi card: eGON {egon!r}, partition at {part}, file bytes sha256 {hashlib.sha256(old[:FILE_SIZE]).hexdigest()}")
    if egon != b"eGON.BT0" or part != PI_PART_OFF:
        raise RuntimeError("not the Orange Pi card -- nothing written")
    if hashlib.sha256(old[:FILE_SIZE]).hexdigest() != OLD_SHA:
        if old[:FILE_SIZE] == new:
            log("Pi card: the new nvme-auto is already there -- nothing to do"); return
        raise RuntimeError("the bytes there are not the known nvme-auto -- nothing written")
    data = new + old[FILE_SIZE:]                      # keep the slack after end-of-file as it is
    letter = ps(f"(Get-Partition -DiskNumber {PI_DISK} -PartitionNumber 1).DriveLetter").strip()
    vol = None
    if letter:                                        # Windows mounted the ext4 partition as a RAW volume
        vol = opn(r"\\.\%s:" % letter, GR | GW)
        r = wt.DWORD(0)
        ok_l = k32.DeviceIoControl(vol, FSCTL_LOCK_VOLUME, None, 0, None, 0, ctypes.byref(r), None)
        ok_d = k32.DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, None, 0, None, 0, ctypes.byref(r), None)
        log(f"Pi card: volume {letter}: locked={bool(ok_l)} dismounted={bool(ok_d)}")
        if not ok_l:
            k32.CloseHandle(vol); raise RuntimeError("could not lock the volume -- nothing written")
    try:
        hw = opn(r"\\.\PhysicalDrive%d" % PI_DISK, GR | GW, NOBUF | WTHRU)
        try:
            ctypes.memmove(buf, data, len(data))
            k32.SetFilePointerEx(hw, FILE_OFF, None, 0)
            w = wt.DWORD(0)
            if not k32.WriteFile(hw, buf, len(data), ctypes.byref(w), None) or w.value != len(data):
                raise OSError(f"write failed: winerr {ctypes.get_last_error()}")
            k32.FlushFileBuffers(hw)
        finally:
            k32.CloseHandle(hw)
    finally:
        if vol:
            k32.CloseHandle(vol)
    time.sleep(2)
    hr = opn(r"\\.\PhysicalDrive%d" % PI_DISK, GR)
    try:
        back = pread(hr, FILE_OFF, FILE_BLOCKS * 4096, buf)
    finally:
        k32.CloseHandle(hr)
    if back != data:
        raise RuntimeError("READ-BACK MISMATCH on the Pi card")
    log(f"Pi card: nvme-auto rewritten in place and read back identical (sha256 {hashlib.sha256(new).hexdigest()})")


def main():
    rc = 0
    log(f"elevated: {bool(ctypes.windll.shell32.IsUserAnAdmin())}; FPGA disk {FPGA_DISK}, Pi disk {PI_DISK}")
    for name, fn in (("FPGA card #1", fpga_card), ("Pi card", pi_card)):
        try:
            fn()
        except Exception as e:
            log(f"{name}: FAILED: {e}"); rc = 1
    log(f"DONE rc={rc}")


if __name__ == "__main__":
    main()
