#!/usr/bin/env python3
"""verify_cards.py -- READ-ONLY check of the two cards after write_cards.py (run elevated). Never writes.

write_cards.py already compared raw bytes: card #1's whole image by sha256, the Pi's 2 blocks at the
offset ext4_read.py --extents gave. This reads the same files back THROUGH the filesystems, by path
(directory lookup -> inode -> extents), the way the boards will:
  * Pi card: /usr/local/sbin/nvme-auto must hash to the tested file (accel/pi/out/nvme-auto.inplace),
    and still be 7959 bytes; /etc/fstab is printed (it holds the stale /mnt/nvme vfat line the new
    nvme-auto replaces at the Pi's next boot);
  * card #1: the guard and its unit on the root filesystem must equal the repo's files.

  Start-Process python -Verb RunAs -Wait -ArgumentList '"D:\\espicpc\\machine\\cards\\verify_cards.py" 3 4'
  log: D:\\start\\machine-cards\\evidence\\verify_cards.log, last line 'DONE rc=0'
"""
import hashlib
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "hardware", "pz7020-starlite", "linux"))
import ext4_read  # noqa: E402

FPGA_DISK, PI_DISK = int(sys.argv[1]), int(sys.argv[2])
LOG = r"D:\start\machine-cards\evidence\verify_cards.log"
PI_LBA, CARD1_LBA = 65536, 135266304 // 512


def log(m):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S  ") + m + "\n")


def sha(b):
    return hashlib.sha256(b).hexdigest()


def read_file(fs, path):
    ino = fs.lookup(path)
    if ino is None:
        return None, None
    return ino, fs.data(ino)[:fs.size(ino)]


def main():
    rc = 0
    pi = ext4_read.Ext4(ext4_read.Disk(r"\\.\PhysicalDrive%d" % PI_DISK, PI_LBA))
    want = open(os.path.join(REPO, "accel", "pi", "out", "nvme-auto.inplace"), "rb").read()
    ino, got = read_file(pi, "/usr/local/sbin/nvme-auto")
    if got is None:
        log("Pi card: /usr/local/sbin/nvme-auto NOT FOUND"); rc = 1
    else:
        same = got == want
        log(f"Pi card: /usr/local/sbin/nvme-auto inode {ino}, {len(got)} bytes, sha256 {sha(got)} -- "
            + ("SAME as the tested file" if same else f"DIFFERENT from the tested file ({sha(want)})"))
        rc |= 0 if same and len(got) == 7959 else 1
    _, fstab = read_file(pi, "/etc/fstab")
    for ln in (fstab or b"").decode("utf-8", "replace").splitlines():
        if ln.strip() and not ln.lstrip().startswith("#") or "nvme-auto" in ln:
            log("Pi card: fstab | " + ln)
    c1 = ext4_read.Ext4(ext4_read.Disk(r"\\.\PhysicalDrive%d" % FPGA_DISK, CARD1_LBA))
    for path, src in (("/usr/local/sbin/zynq-plcheck", "zynq-plcheck"),
                      ("/etc/systemd/system/zynq-plcheck.service", "zynq-plcheck.service"),
                      ("/etc/systemd/system/fpgagpud.service.d/plcheck.conf", "plcheck-requires.conf"),
                      ("/etc/systemd/journald.conf.d/bringup.conf", "journald-bringup.conf")):
        ref = open(os.path.join(REPO, "hardware", "pz7020-starlite", "linux", src), "rb").read().replace(b"\r\n", b"\n")
        ino, got = read_file(c1, path)
        ok = got == ref
        log(f"card #1: {path}: " + ("SAME as linux/" + src if ok else ("NOT FOUND" if got is None else "DIFFERENT")))
        rc |= 0 if ok else 1
    log(f"DONE rc={rc}")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        log(f"ERROR {e!r}"); log("DONE rc=1")
