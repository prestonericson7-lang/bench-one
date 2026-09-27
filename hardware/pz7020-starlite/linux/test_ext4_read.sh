#!/bin/bash
# test_ext4_read.sh -- ext4_read.py (the read-only reader used to inspect a board's card from Windows) against
# the kernel's own ext4 driver on the Pi card image: file bytes (small, multi-extent, > 128 MiB with full-length
# extents), symlinks in the middle and at the end of a path, and a directory listing. WSL, root.
set -u
IMG=${IMG:-/mnt/d/espicpc/accel/pi-card/opi4pro-accel.img}
RD=/mnt/d/espicpc/hardware/pz7020-starlite/linux/ext4_read.py
[ -f "$IMG" ] || { echo "no Pi card image at $IMG (accel/build_pi_card_image.sh)"; exit 1; }
M=$(mktemp -d); L=$(losetup -f --show -r -o $((32 * 1024 * 1024)) "$IMG")
trap 'mountpoint -q "$M" && umount "$M"; losetup -d "$L"; rmdir "$M"' EXIT
mount -o ro "$L" "$M" || exit 1
PASS=0; FAIL=0
chk() { if eval "$2"; then echo "  PASS  $1"; PASS=$((PASS + 1)); else echo "  FAIL  $1"; FAIL=$((FAIL + 1)); fi; }
rd_sha() { python3 - "$RD" "$IMG" "$1" <<'EOF'
import hashlib, importlib.util, sys
spec = importlib.util.spec_from_file_location("e", sys.argv[1]); e = importlib.util.module_from_spec(spec); spec.loader.exec_module(e)
fs = e.Ext4(e.Disk(sys.argv[2], 65536))
print(hashlib.sha256(fs.data(fs.lookup(sys.argv[3], follow_last=True))).hexdigest())
EOF
}
for p in /etc/hostname /var/lib/dpkg/status /usr/bin/bash /boot/uInitrd-6.6.98-sun60iw2 /opt/accel/models/qwen2.5-coder-3b.gguf; do
  chk "bytes of $p ($(stat -c %s "$M$p") B) = the kernel's" "[ \"\$(rd_sha $p)\" = \"\$(sha256sum < $M$p | cut -d' ' -f1)\" ]"
done
chk "symlink at the end: /boot/uInitrd -> the same bytes" "[ \"\$(rd_sha /boot/uInitrd)\" = \"\$(sha256sum < $M/boot/uInitrd | cut -d' ' -f1)\" ]"
chk "symlink in the middle: /bin/bash through /bin -> usr/bin" "[ \"\$(rd_sha /bin/bash)\" = \"\$(sha256sum < $M/bin/bash | cut -d' ' -f1)\" ]"
D=/etc/systemd/system/multi-user.target.wants
chk "directory listing of $D = ls" "[ \"\$(python3 $RD $IMG 65536 $D/ | tail -n +2 | awk '{ print \$2 }' | sort)\" = \"\$(ls -A $M$D | sort)\" ]"
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ] && echo "EXT4 READER TEST: PASS" || echo "EXT4 READER TEST: FAIL"
[ "$FAIL" = 0 ]
