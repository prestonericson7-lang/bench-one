#!/usr/bin/env python3
"""read_pi_more.py -- READ-ONLY second look at the Orange Pi card (run elevated). Never writes.

  * the root filesystem's superblock: features (needs_recovery = not cleanly unmounted), the default
    mount options (journal mode: an in-place DATA edit is safe in ordered/writeback mode only), commit
  * the Pi's own logs as last synced to the card (orangepi-ramlog keeps /var/log in RAM and syncs it):
    syslog / kern.log / dmesg / boot.log / orangepi-ramlog.log -- every line about the NVMe, nvme-auto,
    udisks, the wired port (end0), the FPGA link, zaccel-swap, the Teensy
  * /etc/netplan/ (file names only: Wi-Fi profiles live there on this Ubuntu, with their passwords)

  Start-Process python -Verb RunAs -Wait -ArgumentList '"D:\\espicpc\\machine\\cards\\read_pi_more.py" 4'
  out: D:\\start\\machine-cards\\evidence\\pi-more.txt, log line 'DONE rc=0' in read_cards.log
"""
import contextlib
import io
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "hardware", "pz7020-starlite", "linux")))
import ext4_read  # noqa: E402

DISK = int(sys.argv[1])
OUT = r"D:\start\machine-cards\evidence"
LOG = os.path.join(OUT, "read_cards.log")
PART_LBA = 65536                                   # 32 MiB, checked below against the eGON card identity
KEYS = re.compile(r"nvme|nvme-auto|udisk|ntfs|bitlocker|vfat|/mnt/nvme|end0|10\.77|fpgagpu|zaccel|nbd|teensy|"
                  r"ttyACM|16c0|EXT4-fs \(mmc|Wi-?Fi|wlan|NetworkManager.*(state|activat)", re.I)


def log(m):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S  ") + "pi-more: " + m + "\n")


def main():
    src = r"\\.\PhysicalDrive%d" % DISK
    d = ext4_read.Disk(src, 0)
    egon = d.read(8196, 8)
    if egon != b"eGON.BT0":
        log(f"not the Orange Pi card (eGON {egon!r}) -- nothing read"); log("DONE rc=1"); return
    sb = ext4_read.Disk(src, PART_LBA).read(1024, 1024)
    if struct.unpack_from("<H", sb, 0x38)[0] != 0xEF53:
        log("no ext4 superblock at 32 MiB"); log("DONE rc=1"); return
    compat, incompat, ro = struct.unpack_from("<III", sb, 0x5C)
    defm = struct.unpack_from("<I", sb, 0x100)[0]
    mnt_cnt, max_cnt = struct.unpack_from("<Hh", sb, 0x34)
    state, errors = struct.unpack_from("<HH", sb, 0x3A)
    jmode = {0: "none set (kernel default: ordered)", 0x20: "data=journal", 0x40: "data=ordered", 0x60: "data=writeback"}[defm & 0x60]
    opts = struct.unpack_from("64s", sb, 0x200)[0].split(b"\0")[0].decode("ascii", "replace")
    lines = ["== root filesystem superblock",
             f"features compat 0x{compat:x} incompat 0x{incompat:x} ro_compat 0x{ro:x}",
             f"needs_recovery (not cleanly unmounted): {bool(incompat & 0x4)}   has_journal: {bool(compat & 0x4)}   "
             f"metadata_csum: {bool(ro & 0x400)}   inline_data: {bool(incompat & 0x8000)}",
             f"default mount options 0x{defm:x}: journal mode {jmode}   s_mount_opts: '{opts}'",
             f"state 0x{state:x}  mount count {mnt_cnt}"]
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        sys.argv = ["ext4_read.py", src, str(PART_LBA), "/etc/netplan/", "/var/log/orangepi-ramlog.log",
                    "/var/log/boot.log", "/var/log/syslog", "/var/log/kern.log", "/var/log/dmesg"]
        try:
            ext4_read.main()
        except SystemExit as e:
            print(f"ext4_read stopped: {e}")
    text = buf.getvalue()
    # keep whole the small files; from the big logs keep only the lines that matter here
    cur, keep = None, []
    for ln in text.splitlines():
        if ln.startswith("===== "):
            cur = ln; keep.append(""); keep.append(ln); continue
        small = cur and any(s in cur for s in ("/etc/netplan/", "orangepi-ramlog", "boot.log"))
        if small or KEYS.search(ln):
            keep.append(ln)
    open(os.path.join(OUT, "pi-more.txt"), "w", encoding="utf-8").write("\n".join(lines + keep) + "\n")
    log(f"superblock + {len(keep)} log lines written to pi-more.txt")
    log("DONE rc=0")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        log(f"ERROR {e!r}"); log("DONE rc=1")
