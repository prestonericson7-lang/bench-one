#!/bin/bash
# zynq/qemu_boot_test.sh -- boot a COPY of the FPGA-GPU SD image in QEMU (xilinx-zynq-a9) and check
# what can be checked without the board. Run in WSL as root after make_sd_image.sh:
#     bash qemu_boot_test.sh
# Two boots of the same image copy, kernel + dtb taken from the image's FAT partition (QEMU starts
# the kernel directly: U-Boot/boot.scr is not exercised here), the ext4 root from the image:
#   run "dhcp":   QEMU user network with a DHCP server; host port forwarded to the guest's 7777
#   run "nodhcp": network frames go to an unused UDP port -- no DHCP server answers
# Checked from the console log and over TCP:
#   - the kernel accepted /reserved-memory/gpu@1e000000 (no-map) and did not use it
#   - /dev/mem maps the GPU window 0x1E000000..0x1FFFFFFF with O_SYNC and it reads back what was
#     written (the access path fpgagpud uses; first and last 8 bytes, plus FB1/pool/RET offsets)
#   - networking.service finishes quickly; eth0 has 10.77.0.2/24 (label eth0:gpu) in both runs and
#     a DHCP lease in the dhcp run
#   - fpgagpud.service is active, listens on 7777, has logged that the PL is not configured (QEMU
#     has no PL), and answers NET_HELLO / NET_STATUS with GPU_ERR_NOPL over the forwarded port
# Test-only deviation: QEMU's zynq machine puts the GEM0 PHY model at MDIO address 7 (the board's
# RTL8211F is at 1), so the QEMU copy of the dtb gets ethernet-phy reg = <7> (fdtput); everything
# else is the image's own dtb.
# Not covered: U-Boot (boot.scr, fdt_high, fpga loadb), the real PHY/cable, the PL.
# Files: $OUT/qemu/ (image copy deleted at the end, console logs kept). Test-only additions go into
# the COPY only (a oneshot check service); the image itself is not modified.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ZYNQ=${ZYNQ:-/root/zynq}
OUT=${OUT:-$ZYNQ/gpu-out}
IMG=$OUT/pz7020-gpu-sd.img
Q=$OUT/qemu
BOOT_TIMEOUT=${BOOT_TIMEOUT:-240}
die() { echo "[qemu_boot_test] ERROR: $*" >&2; exit 1; }
log() { echo "[qemu_boot_test] $*"; }
[ "$(id -u)" = 0 ] || die "run as root"
command -v qemu-system-arm >/dev/null || die "qemu-system-arm missing"
command -v fdtput >/dev/null || die "fdtput missing (device-tree-compiler)"
[ -f "$IMG" ] || die "missing $IMG (run make_sd_image.sh first)"
mkdir -p "$Q"
rm -f "$Q"/*.log "$Q/test.img"

# ---- test-only check service (goes into the image COPY only) --------------------------------------
cat > "$Q/zq-check.sh" <<'EOS'
#!/bin/sh
# test-only: runs once in the QEMU copy, prints ZQ lines on the console
p() { sed 's/^/ZQ /'; }
echo "ZQ BEGIN"
echo "uptime $(cut -d' ' -f1 /proc/uptime)" | p
cat /proc/cmdline | p
dmesg | grep -i -E "reserved mem|gpu@1e000000|Memory:" | p
grep -i -E "^1e000000|^ *1e000000|reserved" /proc/iomem | p
for u in networking.service fpgagpud.service; do
    a=$(systemctl show -p InactiveExitTimestampMonotonic --value $u)
    b=$(systemctl show -p ActiveEnterTimestampMonotonic --value $u)
    echo "activation $u $(( (b - a) / 1000 )) ms" | p
done
echo "dhclient: $(pgrep -a dhclient | head -1)" | p
echo "networking $(systemctl is-active networking.service)" | p
echo "fpgagpud $(systemctl is-active fpgagpud.service) enabled=$(systemctl is-enabled fpgagpud.service)" | p
ip -4 addr show dev eth0 | p
ss -ltnp 2>/dev/null | grep 7777 | p
journalctl -b -u fpgagpud.service --no-pager -o cat 2>/dev/null | head -12 | p
python3 - <<'EOP' 2>&1 | p
import mmap, os, struct
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x2000000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0x1E000000)
ok = True
for off in (0, 0x200000, 0x400000, 0x1E00000, 0x1FFFFF8):
    v = 0x1122334455667788 ^ off
    struct.pack_into("<Q", m, off, v)
    ok = ok and struct.unpack_from("<Q", m, off)[0] == v
print("devmem GPU window 0x1E000000+32MB O_SYNC write/read", "OK" if ok else "MISMATCH")
EOP
echo "ZQ END"
EOS
cat > "$Q/zq-check.service" <<'EOS'
[Unit]
Description=QEMU boot test checks (test copy only)
After=fpgagpud.service networking.service
[Service]
Type=oneshot
ExecStartPre=/bin/sleep 20
ExecStart=/bin/sh /usr/local/bin/zq-check.sh
StandardOutput=journal+console
StandardError=journal+console
[Install]
WantedBy=multi-user.target
EOS
# a fresh copy per boot: power-of-two size for QEMU's SD card model, plus the check service
export MTOOLS_SKIP_CHECK=1
prep() {
    local p2s p2n
    rm -f "$Q/test.img"
    cp --sparse=always "$IMG" "$Q/test.img"
    truncate -s 2G "$Q/test.img"
    read -r p2s p2n < <(sfdisk -d "$Q/test.img" |
                        sed -n 's|^.*img2 : start= *\([0-9]*\), size= *\([0-9]*\),.*|\1 \2|p')
    [ -n "$p2n" ] || die "cannot read the test copy's partition table"
    rm -f "$Q/zImage" "$Q/board.dtb"
    mcopy -n -i "$Q/test.img@@1M" ::/zImage "$Q/zImage"
    mcopy -n -i "$Q/test.img@@1M" ::/zynq-pz7020-starlite.dtb "$Q/board.dtb"
    fdtput -t x "$Q/board.dtb" /axi/ethernet@e000b000/ethernet-phy@1 reg 7   # QEMU's PHY address
    dd if="$Q/test.img" of="$Q/p2.img" bs=512 skip="$p2s" count="$p2n" conv=sparse status=none
    dbg() { debugfs -w -R "$1" "$Q/p2.img" >/dev/null 2>&1 || true; }
    dbg "write $Q/zq-check.sh /usr/local/bin/zq-check.sh"
    dbg "sif /usr/local/bin/zq-check.sh mode 0100755"
    dbg "write $Q/zq-check.service /etc/systemd/system/zq-check.service"
    dbg "sif /etc/systemd/system/zq-check.service mode 0100644"
    dbg "symlink /etc/systemd/system/multi-user.target.wants/zq-check.service /etc/systemd/system/zq-check.service"
    dd if="$Q/p2.img" of="$Q/test.img" bs=512 seek="$p2s" conv=notrunc,sparse status=none
    rm -f "$Q/p2.img"
}

APPEND="console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0"
FAILS=0
ck() { if eval "$2"; then echo "  ok    $1"; else echo "  FAIL  $1"; FAILS=$((FAILS + 1)); fi; }

hello() {   # hello PORT -> prints "status proto" of NET_HELLO and "status nregs" of NET_STATUS
    python3 - "$1" <<'EOP'
import socket, struct, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
def req(t):
    s.sendall(struct.pack("<IHHI", 0x31504746, t, 0, 0))
    h = b""
    while len(h) < 12:
        h += s.recv(12 - len(h))
    magic, rt, _, n = struct.unpack("<IHHI", h)
    b = b""
    while len(b) < n:
        b += s.recv(n - len(b))
    assert magic == 0x31504746 and rt == (t | 0x8000), (hex(magic), hex(rt))
    return b
h = req(1)
st = req(10)
print(struct.unpack_from("<iI", h)[0], struct.unpack_from("<iI", h)[1], struct.unpack_from("<i", st)[0], (len(st) - 16) // 4)
EOP
}

boot() {    # boot NAME NETARGS...
    local name=$1; shift
    local con=$Q/console-$name.log
    log "boot '$name' (up to ${BOOT_TIMEOUT}s)"
    timeout "$BOOT_TIMEOUT" qemu-system-arm -M xilinx-zynq-a9 -m 512M -display none -monitor none \
        -serial "file:$con" -kernel "$Q/zImage" -dtb "$Q/board.dtb" -append "$APPEND" \
        -drive "file=$Q/test.img,if=sd,format=raw" "$@" &
    QPID=$!
    local t=0
    while [ $t -lt "$BOOT_TIMEOUT" ] && kill -0 $QPID 2>/dev/null; do
        grep -q "ZQ END" "$con" 2>/dev/null && break
        sleep 2; t=$((t + 2))
    done
}
stop() { kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true; }

# ---- run 1: DHCP server present, host port forwarded ------------------------------------------------
HP=$((20000 + RANDOM % 20000))
prep
boot dhcp -nic "user,hostfwd=tcp:127.0.0.1:$HP-:7777"
C=$Q/console-dhcp.log
R=$(hello "$HP" 2>&1 || true)
stop
grep "ZQ " "$C" | sed 's/\r//' > "$Q/zq-dhcp.log" || true
cat "$Q/zq-dhcp.log"
echo "  host -> guest 7777: HELLO status/proto, STATUS status/nregs = $R"
ck "dhcp: check script ran to the end"             "grep -q 'ZQ END' '$Q/zq-dhcp.log'"
ck "dhcp: kernel reserved gpu@1e000000 (no-map)"   "grep -q -i 'reserved mem: 0x1e000000.*nomap.*gpu@1e000000' '$Q/zq-dhcp.log'"
ck "dhcp: /dev/mem O_SYNC map of the GPU window"   "grep -q 'devmem GPU window .* OK' '$Q/zq-dhcp.log'"
ck "dhcp: networking.service active"               "grep -q 'ZQ networking active' '$Q/zq-dhcp.log'"
ck "dhcp: eth0 has 10.77.0.2/24 (eth0:gpu)"        "grep -q 'inet 10.77.0.2/24 .*eth0:gpu' '$Q/zq-dhcp.log'"
ck "dhcp: eth0 link up"                           "grep -q 'eth0: <BROADCAST,MULTICAST,UP,LOWER_UP>' '$Q/zq-dhcp.log'"
ck "dhcp: eth0 also has a DHCP lease (10.0.2.x)"   "grep -q 'inet 10.0.2.[0-9]*/24 .* eth0\$' '$Q/zq-dhcp.log'"
ck "dhcp: fpgagpud active + enabled"               "grep -q 'ZQ fpgagpud active enabled=enabled' '$Q/zq-dhcp.log'"
ck "dhcp: fpgagpud listening on 7777"              "grep -q ':7777 ' '$Q/zq-dhcp.log'"
ck "dhcp: fpgagpud logged PL not configured"       "grep -q 'PL not ready' '$Q/zq-dhcp.log'"
ck "dhcp: HELLO/STATUS over TCP -> GPU_ERR_NOPL, proto 1, 24 regs" "[ '$R' = '-2 1 -2 24' ]"

# ---- run 2: no DHCP server -----------------------------------------------------------------------------
UP=$((40000 + RANDOM % 20000))
prep
boot nodhcp -nic "socket,udp=127.0.0.1:$UP,localaddr=127.0.0.1:$((UP + 1))"
stop
C=$Q/console-nodhcp.log
grep "ZQ " "$C" | sed 's/\r//' > "$Q/zq-nodhcp.log" || true
cat "$Q/zq-nodhcp.log"
ck "nodhcp: check script ran to the end"           "grep -q 'ZQ END' '$Q/zq-nodhcp.log'"
ck "nodhcp: networking.service active"             "grep -q 'ZQ networking active' '$Q/zq-nodhcp.log'"
ck "nodhcp: eth0 has 10.77.0.2/24 without DHCP"    "grep -q 'inet 10.77.0.2/24 .*eth0:gpu' '$Q/zq-nodhcp.log'"
ck "nodhcp: no DHCP lease"                         "! grep -q 'inet 10.0.2' '$Q/zq-nodhcp.log'"
NW=$(sed -n 's/^.*ZQ activation networking.service \([0-9]*\) ms.*/\1/p' "$Q/zq-nodhcp.log" | head -1)
echo "  networking.service activation without DHCP: ${NW:-unknown} ms"
ck "nodhcp: networking.service activation < 10 s (no DHCP wait)" "[ -n '$NW' ] && [ '$NW' -lt 10000 ]"
ck "nodhcp: dhclient keeps running in the background" "grep -q 'ZQ dhclient: [0-9]* dhclient -4 -nw' '$Q/zq-nodhcp.log'"
ck "nodhcp: fpgagpud active"                       "grep -q 'ZQ fpgagpud active' '$Q/zq-nodhcp.log'"
rm -f "$Q/test.img" "$Q/zImage" "$Q/board.dtb"
[ "$FAILS" = 0 ] || die "$FAILS check(s) failed (console logs in $Q)"
log "all checks passed (console logs in $Q)"
