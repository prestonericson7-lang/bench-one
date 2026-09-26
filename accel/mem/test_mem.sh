#!/bin/bash
# test_mem.sh -- the zynqram export and zaccel-swap end to end, inside WSL (Ubuntu 22.04, as root):
#   MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-22.04 -u root -- bash /mnt/d/espicpc/accel/mem/test_mem.sh | tr -d '\0'
#
# Zynq side, for real under WSL's systemd: install_zynq.sh /, the package's nbd-server reading
# conf.d/zynqram.conf, zynqram-prep.service making the tmpfs file, TCP 10809 on 127.0.0.1.
# Pi side: zaccel-swap from this directory with ZYNQ_HOST=127.0.0.1, then install_pi.sh and its units.
# Throughput figures are PC loopback (WSL talking to itself), NOT board numbers.
# Everything is undone at the end; only the nbd-server and nbd-client packages stay installed.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ZS="bash $HERE/zaccel-swap"
T=/tmp/zaccel-mem-test
# the export size is whatever the shipped default says (zynqram.default), not a number in this test
EXPORT_MB=$(sed -n 's/^ZACCEL_SWAP_MB=\([0-9]*\)$/\1/p' "$HERE/zynq/zynqram.default")
EXPORT_BYTES=$((EXPORT_MB * 1048576))
PASS=0; FAIL=0; RESULTS=()
ok()  { PASS=$((PASS + 1)); RESULTS+=("PASS  $1"); echo "  PASS: $1"; }
bad() { FAIL=$((FAIL + 1)); RESULTS+=("FAIL  $1"); echo "  FAIL: $1"; }
expect() { local name=$1; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }
hdr() { echo; echo "=== $*"; }
now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$(now)" 'BEGIN { printf "%.1f", b - a }'; }
nbd_swaps() { awk 'NR > 1 && $1 ~ /^\/dev\/nbd/' /proc/swaps; }
swap_prio() { awk -v d="$1" 'NR > 1 && $1 == d { print $5 }' /proc/swaps; }
swap_used_kib() { awk -v d="$1" 'NR > 1 && $1 == d { print $4 }' /proc/swaps; }
cur_dev() { cat /run/zaccel-swap/device 2>/dev/null; }
disk_sig() { dd if="$1" bs=4096 count=1 iflag=direct status=none 2>/dev/null | tail -c 10 | tr -d '\0'; }
lt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a < b) }'; }
between() { awk -v x="$1" -v a="$2" -v b="$3" 'BEGIN { exit !(x >= a && x <= b) }'; }

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

# ---------------------------------------------------------------- state to restore afterwards
SWAPS_BEFORE=$(cat /proc/swaps)
NBD_WAS_LOADED=0; [ -d /sys/module/nbd ] && NBD_WAS_LOADED=1
CLEANED=0
HOG_PID=""
OURS_ZYNQ="/etc/default/zynqram /etc/nbd-server/conf.d/zynqram.conf /usr/local/sbin/zynqram-prep
  /etc/systemd/system/zynqram-prep.service /etc/systemd/system/nbd-server.service.requires/zynqram-prep.service
  /etc/systemd/system/multi-user.target.wants/zynqram-prep.service"
OURS_PI="/usr/local/sbin/zaccel-swap /etc/modules-load.d/nbd.conf /etc/systemd/system/zaccel-swap.service
  /etc/systemd/system/zaccel-swap-retry.service /etc/systemd/system/zaccel-swap-retry.timer
  /etc/systemd/system/multi-user.target.wants/zaccel-swap.service"

stop_zynq() {   # the package's init script only kills the parent; connection children are separate
  systemctl stop nbd-server 2>/dev/null
  pkill -CONT -x nbd-server 2>/dev/null
  pkill -x nbd-server 2>/dev/null
  for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x nbd-server >/dev/null || break; sleep 0.3; done
}

cleanup() {
  [ "$CLEANED" = 1 ] && return
  CLEANED=1
  set +e
  [ -n "$HOG_PID" ] && kill -9 "$HOG_PID" 2>/dev/null && wait "$HOG_PID" 2>/dev/null
  if [ -e /etc/systemd/system/zaccel-swap.service ]; then
    systemctl stop zaccel-swap.service zaccel-swap-retry.timer zaccel-swap-retry.service 2>/dev/null
    systemctl disable zaccel-swap.service >/dev/null 2>&1
  fi
  rm -rf /etc/systemd/system/zaccel-swap.service.d /etc/systemd/system/zaccel-swap-retry.service.d
  rm -f $OURS_PI
  $ZS stop >/dev/null 2>&1
  for s in $(nbd_swaps | awk '{ print $1 }'); do swapoff "$s"; done
  for d in /sys/block/nbd*; do [ -e "$d/pid" ] && nbd-client -d "/dev/${d##*/}" >/dev/null 2>&1; done
  stop_zynq
  systemctl stop zynqram-prep 2>/dev/null
  mountpoint -q /run/zynqram && umount /run/zynqram
  rmdir /run/zynqram 2>/dev/null
  rm -f $OURS_ZYNQ
  rmdir /etc/systemd/system/nbd-server.service.requires 2>/dev/null
  systemctl daemon-reload
  systemctl reset-failed zynqram-prep.service zaccel-swap.service zaccel-swap-retry.service nbd-server.service 2>/dev/null
  [ "${NBDSRV_WAS:-}" = active ] && systemctl start nbd-server >/dev/null 2>&1   # the package's no-export state
  rm -rf /run/zaccel-swap "$T"
  if [ "$NBD_WAS_LOADED" = 0 ] && [ -d /sys/module/nbd ]; then
    for _ in 1 2 3 4 5 6 7 8 9 10; do rmmod nbd 2>/dev/null && break; sleep 0.5; done
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- packages
hdr "0. packages (WSL is the test host: jammy's nbd 3.23; the Zynq gets bookworm's 3.24, the Pi resolute's 3.26.1)"
export DEBIAN_FRONTEND=noninteractive
if ! dpkg-query -W nbd-server nbd-client >/dev/null 2>&1; then
  apt-get install -y nbd-server nbd-client >/dev/null 2>&1 || { apt-get update >/dev/null && apt-get install -y nbd-server nbd-client >/dev/null; }
fi
dpkg-query -W -f '  ${Package} ${Version}\n' nbd-server nbd-client
NBDSRV_WAS=$(systemctl is-active nbd-server 2>/dev/null)
echo "  nbd-server.service before the test: $NBDSRV_WAS; nbd module loaded before: $NBD_WAS_LOADED"
echo "  /proc/swaps before:"; sed 's/^/    /' <<< "$SWAPS_BEFORE"
if pgrep -x nbd-server >/dev/null || pgrep -x nbd-client >/dev/null; then
  echo "  nbd-server/nbd-client processes already running; refusing to touch them"; trap - EXIT; exit 1
fi
rm -rf "$T"; mkdir -p "$T"

# ---------------------------------------------------------------- B. Zynq side
hdr "B. Zynq side: install_zynq.sh, zynqram-prep.service, the package's nbd-server"
S=$T/stage; mkdir -p "$S"
bash "$HERE/install_zynq.sh" "$S" | sed 's/^/  /'
bash "$HERE/install_zynq.sh" "$S" | sed 's/^/  /'
got=$(cd "$S" && find . \( -type f -o -type l \) | sort | tr '\n' ' ')
echo "  staged: $got"
want="./etc/default/zynqram ./etc/nbd-server/conf.d/zynqram.conf ./etc/nbd-server/zynqram.allow ./etc/systemd/system/multi-user.target.wants/zynqram-prep.service ./etc/systemd/system/nbd-server.service.requires/zynqram-prep.service ./etc/systemd/system/zynqram-prep.service ./usr/local/sbin/zynqram-prep "
expect "install_zynq.sh into a staging rootfs (run twice) gives 5 files + 2 enable links" [ "$got" = "$want" ]
# with the package's main config present (as in the image), the listener goes IPv4-only exactly once
S2=$(mktemp -d); mkdir -p "$S2/etc/nbd-server"
printf '[generic]\n\tuser = nbd\n\tgroup = nbd\n\tincludedir = /etc/nbd-server/conf.d\n' > "$S2/etc/nbd-server/config"
bash "$HERE/install_zynq.sh" "$S2" >/dev/null 2>&1; bash "$HERE/install_zynq.sh" "$S2" >/dev/null 2>&1
expect "install_zynq.sh sets listenaddr = 0.0.0.0 in [generic], once (the allow list depends on it)" \
  [ "$(grep -c '^[[:space:]]*listenaddr = 0.0.0.0' "$S2/etc/nbd-server/config")" = 1 ]
rm -rf "$S2"
expect "enable link points at /etc/systemd/system/zynqram-prep.service" \
  [ "$(readlink "$S/etc/systemd/system/nbd-server.service.requires/zynqram-prep.service")" = /etc/systemd/system/zynqram-prep.service ]

bash "$HERE/install_zynq.sh" / | sed 's/^/  /'
systemctl daemon-reload
systemctl stop nbd-server                      # the package's instance, which had no exports
t0=$(now); systemctl start nbd-server; echo "  systemctl start nbd-server: rc=$? in $(since "$t0") s"
for _ in $(seq 1 20); do ss -ltn | grep -q ':10809 ' && break; sleep 0.25; done
echo "  nbd-server.service Requires: $(systemctl show -p Requires --value nbd-server)"
echo "  nbd-server.service After:    $(systemctl show -p After --value nbd-server | tr ' ' '\n' | grep zynqram)"
journalctl -u zynqram-prep --no-pager -n 3 -o cat | sed 's/^/  prep: /'
expect "nbd-server.service requires and is ordered after zynqram-prep.service" \
  bash -c 'systemctl show -p Requires --value nbd-server | grep -qw zynqram-prep.service && systemctl show -p After --value nbd-server | grep -qw zynqram-prep.service'
expect "zynqram-prep.service ran (active)" [ "$(systemctl is-active zynqram-prep)" = active ]
findmnt -n -o SOURCE,FSTYPE,OPTIONS /run/zynqram | sed 's/^/  mount: /'
expect "/run/zynqram is its own tmpfs of ${EXPORT_MB}m" bash -c "findmnt -n -o FSTYPE,OPTIONS /run/zynqram | grep -q '^tmpfs .*size=$((EXPORT_MB * 1024))k'"
F=/run/zynqram/zynqram.img
alloc=$(( $(stat -c %b "$F") * $(stat -c %B "$F") ))
echo "  file: $(stat -c '%s bytes, owner %U:%G, mode %a' "$F"); allocated $alloc bytes; tmpfs used $(df -B1 --output=used /run/zynqram | tail -1) bytes"
expect "export file is $EXPORT_MB MiB, fully allocated, owned by nbd, mode 600" \
  [ "$(stat -c '%s %U %a' "$F")" = "$EXPORT_BYTES nbd 600" -a "$alloc" -ge "$EXPORT_BYTES" ]
ss -ltnp | grep ':10809 ' | sed 's/^/  listen: /'
expect "nbd-server listens on TCP 10809" bash -c "ss -ltn | grep -q ':10809 '"
expect "nbd-server runs as user nbd" [ "$(ps -o user= -C nbd-server | sort -u | tr -d ' ')" = nbd ]

# ---------------------------------------------------------------- C. the raw export
hdr "C. export zynqram as a plain block device: integrity, then throughput (PC loopback)"
modprobe nbd
DEV=/dev/nbd1
out=$(timeout 30 nbd-client -N zynqram -swap -timeout 30 127.0.0.1 10809 $DEV 2>&1); rc=$?
sed 's/^/  nbd-client: /' <<< "$out"
expect "nbd-client -N zynqram -swap -timeout 30 127.0.0.1 10809 $DEV attaches" [ $rc = 0 ]
echo "  nbd-client processes left after attach: '$(pgrep -a -x nbd-client)'  /sys/block/nbd1/pid: '$(cat /sys/block/nbd1/pid 2>/dev/null)'  nbd-client -c: rc=$(nbd-client -c $DEV >/dev/null 2>&1; echo $?)"
expect "netlink form: nbd-client exits after setup, nothing left in the I/O path" bash -c '! pgrep -x nbd-client >/dev/null'
expect "device size is $EXPORT_MB MiB" [ "$(blockdev --getsize64 $DEV)" = "$EXPORT_BYTES" ]
head -c $((64 * 1048576)) /dev/urandom > "$T/pattern"
dd if="$T/pattern" of=$DEV bs=1M oflag=direct conv=fsync status=none
dd if=$DEV of="$T/readback" bs=1M count=64 iflag=direct status=none
expect "64 MiB random pattern written and read back through $DEV (O_DIRECT) matches" cmp -s "$T/pattern" "$T/readback"
expect "the same 64 MiB is in the Zynq-side tmpfs file" cmp -s -n $((64 * 1048576)) "$T/pattern" "$F"
nbd-client -d $DEV >/dev/null 2>&1
timeout 30 nbd-client -N zynqram -swap -timeout 30 127.0.0.1 10809 $DEV >/dev/null 2>&1
dd if=$DEV of="$T/readback2" bs=1M count=64 iflag=direct status=none
expect "pattern still there after detach + re-attach (the export keeps its RAM between connections)" cmp -s "$T/pattern" "$T/readback2"
echo "  throughput, O_DIRECT, PC LOOPBACK (WSL to itself; NOT a board number):"
for spec in "write 1M 256" "read 1M 256" "write 4k 16384" "read 4k 16384"; do
  set -- $spec
  if [ "$1" = write ]; then r=$(dd if=/dev/zero of=$DEV bs=$2 count=$3 oflag=direct 2>&1 | tail -1)
  else r=$(dd if=$DEV of=/dev/null bs=$2 count=$3 iflag=direct 2>&1 | tail -1); fi
  printf '    %-5s bs=%-3s %s\n' "$1" "$2" "$r"
done
nbd-client -d $DEV >/dev/null 2>&1
sleep 0.5
expect "$DEV detached (nbd-client -c says not connected, size 0)" \
  bash -c "! nbd-client -c $DEV >/dev/null 2>&1 && [ \"\$(cat /sys/block/nbd1/size)\" = 0 ]"

# ---------------------------------------------------------------- D. zaccel-swap
hdr "D. zaccel-swap start/stop against the local Zynq stand-in (ZYNQ_HOST=127.0.0.1)"
cat > "$T/hog.py" <<'EOF'
import ctypes, hashlib, os, sys, time
ctypes.CDLL(None).prctl(4, 0, 0, 0, 0)   # PR_SET_DUMPABLE 0: the expected SIGBUS leaves no core dump
mib, go = int(sys.argv[1]), sys.argv[2]
buf = bytearray(mib << 20)
h = hashlib.sha256()
for i in range(mib):
    chunk = os.urandom(1 << 20)
    buf[i << 20:(i + 1) << 20] = chunk
    h.update(chunk)
want = h.hexdigest()
print("ready", flush=True)
while not os.path.exists(go):
    time.sleep(0.2)
got = hashlib.sha256(buf).hexdigest()
print("match" if got == want else "MISMATCH", flush=True)
sys.exit(0 if got == want else 3)
EOF
start_hog() {   # $1 = tag; 96 MiB of random data in a 32 MiB memory cgroup, so most of it must swap
  systemd-run --scope -q -p MemoryMax=32M python3 "$T/hog.py" 96 "$T/go-$1" > "$T/hog-$1.log" 2>&1 &
  HOG_PID=$!
  for _ in $(seq 1 240); do grep -q ready "$T/hog-$1.log" && return 0; kill -0 $HOG_PID 2>/dev/null || return 1; sleep 0.5; done
  return 1
}

out=$(ZYNQ_HOST=127.0.0.1 $ZS start 2>&1); rc=$?
sed 's/^/  /' <<< "$out"
D1=$(cur_dev)
echo "  /proc/swaps:"; sed 's/^/    /' /proc/swaps
expect "start exits 0 and attaches $D1" [ $rc = 0 -a -n "$D1" ]
expect "/proc/swaps shows $D1 at priority 100" [ "$(swap_prio "$D1")" = 100 ]
expect "priority 100 is above the disk swap already there" bash -c "awk 'NR>1 && \$1 !~ /nbd/ && \$5 >= 100 { bad = 1 } END { exit bad }' /proc/swaps"
expect "the attach leaves no nbd-client process (netlink form)" bash -c '! pgrep -x nbd-client >/dev/null'

out=$(ZYNQ_HOST=127.0.0.1 $ZS start 2>&1); rc=$?
sed 's/^/  /' <<< "$out"
grep -q "already active" <<< "$out"; said=$?
expect "second start is a no-op: exit 0, 'already active', still exactly one nbd swap" \
  [ $rc = 0 -a $said = 0 -a "$(nbd_swaps | wc -l)" = 1 ]

echo "  -- swap in use: a 96 MiB process held to 32 MiB of RAM"
if start_hog use; then
  used=$(swap_used_kib "$D1"); echo "  $D1 used while the process sleeps: $used KiB"
  expect "the process's pages went out to the Zynq swap (>= 32 MiB used on $D1)" [ "${used:-0}" -ge 32768 ]
  touch "$T/go-use"; wait $HOG_PID; rc=$?; HOG_PID=""
  echo "  process: $(tr '\n' ' ' < "$T/hog-use.log") rc=$rc"
  grep -q '^match' "$T/hog-use.log"; said=$?
  expect "every page came back from the Zynq swap intact (sha256 match)" [ $rc = 0 -a $said = 0 ]
else
  bad "memory-limited process did not start"; cat "$T/hog-use.log"
fi

echo "  -- Zynq hangs (its nbd-server process stopped, TCP still up): start must fail the old device after -timeout and re-attach"
children=""
for p in $(pgrep -x nbd-server); do     # connection handlers: nbd-server processes forked by nbd-server
  [ "$(ps -o comm= -p "$(ps -o ppid= -p "$p" | tr -d ' ')")" = nbd-server ] && children="$children $p"
done
echo "  stopping nbd-server connection process(es):$children"
kill -STOP $children
t0=$(now); out=$(ZYNQ_HOST=127.0.0.1 $ZS start 2>&1); rc=$?; el=$(since "$t0")
kill -CONT $children
sed 's/^/  /' <<< "$out"
D2=$(cur_dev)
echo "  took $el s; now on $D2, priority $(swap_prio "$D2"), header '$(disk_sig "$D2")'"
expect "hung Zynq: start returns 0 after about the 30 s nbd timeout (took $el s) and re-attaches at priority 100" \
  bash -c "[ $rc = 0 ] && awk -v x=$el 'BEGIN { exit !(x >= 25 && x <= 75) }' && [ '$(swap_prio "$D2")' = 100 ] && [ '$(disk_sig "$D2")' = SWAPSPACE2 ] && [ \$(awk 'NR>1 && \$1 ~ /nbd/' /proc/swaps | wc -l) = 1 ]"

echo "  -- Zynq reboots while pages are out on it: RAM gone, blank export, start must re-attach fresh"
if start_hog reboot; then
  used=$(swap_used_kib "$D2"); echo "  $D2 used: $used KiB"
  stop_zynq
  systemctl stop zynqram-prep
  umount /run/zynqram
  systemctl start nbd-server
  for _ in $(seq 1 20); do ss -ltn | grep -q ':10809 ' && break; sleep 0.25; done
  echo "  Zynq back: export file header '$(dd if=$F bs=4096 count=1 status=none | tail -c 10 | tr -d '\0')' (blank = RAM was lost)"
  t0=$(now); out=$(ZYNQ_HOST=127.0.0.1 $ZS start 2>&1); rc=$?; el=$(since "$t0")
  sed 's/^/  /' <<< "$out"
  D3=$(cur_dev)
  echo "  took $el s; now on $D3, priority $(swap_prio "$D3"), header '$(disk_sig "$D3")'"
  expect "rebooted Zynq: start exits 0, old device swapped off, blank export attached fresh at priority 100" \
    bash -c "[ $rc = 0 ] && [ '$(swap_prio "$D3")' = 100 ] && [ '$(disk_sig "$D3")' = SWAPSPACE2 ] && [ \$(awk 'NR>1 && \$1 ~ /nbd/' /proc/swaps | wc -l) = 1 ]"
  touch "$T/go-reboot"; wait $HOG_PID; rc=$?; HOG_PID=""
  echo "  process whose pages were on the rebooted Zynq: output '$(tr '\n' ' ' < "$T/hog-reboot.log")' rc=$rc"
  expect "that process is killed by a signal, never handed silently wrong data" \
    bash -c "[ $rc -ge 128 ] && ! grep -q -e MISMATCH -e '^match' '$T/hog-reboot.log'"
  dmesg | grep -i -E "nbd|swap" | tail -n 8 | sed 's/^/  dmesg: /'
else
  bad "memory-limited process did not start"; cat "$T/hog-reboot.log"
fi

out=$(ZYNQ_HOST=127.0.0.1 $ZS stop 2>&1); rc=$?
sed 's/^/  /' <<< "$out"
dn=${D3#/dev/}
expect "stop exits 0; no nbd swap left; $D3 disconnected (size 0, nbd-client -c = 1)" \
  bash -c "[ $rc = 0 ] && [ -z \"\$(awk 'NR>1 && \$1 ~ /nbd/' /proc/swaps)\" ] && [ \"\$(cat /sys/block/$dn/size)\" = 0 ] && ! nbd-client -c $D3 >/dev/null 2>&1"
out=$($ZS stop 2>&1); rc=$?
sed 's/^/  /' <<< "$out"
expect "second stop is a no-op, exit 0" [ $rc = 0 ]

echo "  -- no Zynq"
t0=$(now); out=$(ZYNQ_HOST=127.0.0.1 ZYNQ_PORT=10899 $ZS start 2>&1); rc=$?; el=$(since "$t0")
sed 's/^/  /' <<< "$out"
expect "refused port (nothing listening): exit 0 in $el s, nothing attached" \
  bash -c "[ $rc = 0 ] && awk -v x=$el 'BEGIN { exit !(x < 2) }' && [ -z \"\$(awk 'NR>1 && \$1 ~ /nbd/' /proc/swaps)\" ]"
t0=$(now); out=$(ZYNQ_HOST=192.0.2.1 $ZS start 2>&1); rc=$?; el=$(since "$t0")
sed 's/^/  /' <<< "$out"
expect "silent host (192.0.2.1, TEST-NET, packets go nowhere): exit 0 in $el s, nothing attached" \
  bash -c "[ $rc = 0 ] && awk -v x=$el 'BEGIN { exit !(x < 6) }' && [ -z \"\$(awk 'NR>1 && \$1 ~ /nbd/' /proc/swaps)\" ]"

# ---------------------------------------------------------------- E. the Pi's units
hdr "E. install_pi.sh as on the Pi (modprobe nbd fails, prebuilt module), Zynq down; the timer attaches when it comes up"
stop_zynq
for u in zaccel-swap.service zaccel-swap-retry.service; do     # test-only: point the units at 127.0.0.1
  mkdir -p /etc/systemd/system/$u.d
  printf '[Service]\nEnvironment=ZYNQ_HOST=127.0.0.1\n' > /etc/systemd/system/$u.d/test.conf
done
K=$(uname -r)
modsums() { (cd "/lib/modules/$K" && sha256sum modules.* | sha256sum); }
MODSUMS_BEFORE=$(modsums)
mkdir -p "$T/fakebin"
cat > "$T/fakebin/modprobe" <<'EOF'
#!/bin/bash
# fake modprobe: the first $FAKE_MODPROBE_FAILS calls fail like the Pi's kernel, which has no nbd;
# later calls run the real modprobe with -v, so the log shows which file it loaded
F=$(dirname "$0"); n=$(cat "$F/count" 2>/dev/null || echo 0); echo $((n + 1)) > "$F/count"
if [ "$n" -lt "${FAKE_MODPROBE_FAILS:-1}" ]; then
  echo "fake modprobe $*: FATAL: Module $* not found" | tee -a "$F/log" >&2; exit 1
fi
/usr/sbin/modprobe -v "$@" 2>&1 | tee -a "$F/log"; exit "${PIPESTATUS[0]}"
EOF
chmod +x "$T/fakebin/modprobe"
cat > "$T/ns_run.sh" <<'EOF'
#!/bin/bash
# ns_run.sh DIR FAILS -- run DIR/install_pi.sh in a private mount namespace in which /lib/modules/<kver>
# is a throwaway tmpfs overlay (nothing reaches WSL's real module tree), with the fake modprobe on PATH
set -u
D=$1; K=$(uname -r); F=$(dirname "$0")/fakebin
W=$(mktemp -d "$(dirname "$0")/ns.XXXX")
mount -t tmpfs tmpfs "$W" && mkdir "$W/lower" "$W/up" "$W/wk" && mount --bind "/lib/modules/$K" "$W/lower" \
  && mount -t overlay overlay -o "lowerdir=$W/lower,upperdir=$W/up,workdir=$W/wk" "/lib/modules/$K" \
  || { echo "ns: could not set up the throwaway module overlay"; exit 99; }
rm -f "$F/count"
FAKE_MODPROBE_FAILS=$2 PATH="$F:$PATH" bash "$D/install_pi.sh"; rc=$?
echo "ns: /lib/modules/$K/updates holds: '$(ls "/lib/modules/$K/updates" 2>/dev/null | tr '\n' ' ')'"
echo "ns: modinfo -n nbd: $(modinfo -n nbd 2>&1)"
exit $rc
EOF
mkpi() {   # $1: a copy of the Pi-side files, laid out as in accel/mem
  mkdir -p "$1"
  cp "$HERE/install_pi.sh" "$HERE/zaccel-swap" "$HERE/zaccel-swap.service" "$HERE/zaccel-swap-retry.service" \
    "$HERE/zaccel-swap-retry.timer" "$1/"
}
REALKO=/lib/modules/$K/kernel/drivers/block/nbd.ko
mkpi "$T/pi-none"
mkpi "$T/pi-bad"; mkdir -p "$T/pi-bad/pi-kmod/$K"; cp "$REALKO" "$T/pi-bad/pi-kmod/$K/nbd.ko"
echo "$(printf '0%.0s' $(seq 64))  /mnt/d/espicpc/accel/mem/pi-kmod/$K/nbd.ko" > "$T/pi-bad/pi-kmod/$K/nbd.ko.sha256"
mkpi "$T/pi";     mkdir -p "$T/pi/pi-kmod/$K";     cp "$REALKO" "$T/pi/pi-kmod/$K/nbd.ko"
# the same shape build_pi_nbd.sh writes: plain sha256sum output naming its own build path
echo "$(sha256sum < "$REALKO" | awk '{ print $1 }')  /mnt/d/espicpc/accel/mem/pi-kmod/$K/nbd.ko" > "$T/pi/pi-kmod/$K/nbd.ko.sha256"
for f in "$HERE"/pi-kmod/*/nbd.ko; do
  [ -f "$f" ] || continue
  echo "  (info) lead's $(basename "$(dirname "$f")")/nbd.ko: .sha256 hash field $( [ "$(awk '{ print $1; exit }' "$f.sha256" 2>/dev/null)" = "$(sha256sum < "$f" | awk '{ print $1 }')" ] && echo matches || echo 'does not match (or no .sha256 yet)')"
done

echo "  -- modprobe fails, no module built for this kernel"
out=$(unshare -m --propagation private bash "$T/ns_run.sh" "$T/pi-none" 99 2>&1); rc=$?
sed 's/^/  /' <<< "$out"
grep -q "no prebuilt nbd module matches it" <<< "$out"; said=$?
grep -q "updates holds: ''" <<< "$out"; clean=$?
expect "no matching module: install_pi.sh exits 1, says so, installs nothing" \
  [ $rc = 1 -a $said = 0 -a $clean = 0 -a ! -e /etc/modules-load.d/nbd.conf -a ! -e /etc/systemd/system/zaccel-swap.service ]
echo "  -- modprobe fails, module present but its sha256 is wrong"
out=$(unshare -m --propagation private bash "$T/ns_run.sh" "$T/pi-bad" 99 2>&1); rc=$?
sed 's/^/  /' <<< "$out"
grep -q "does not match" <<< "$out"; said=$?
grep -q "updates holds: ''" <<< "$out"; clean=$?
expect "checksum mismatch: install_pi.sh exits 1, says so, module not installed" \
  [ $rc = 1 -a $said = 0 -a $clean = 0 -a ! -e /etc/modules-load.d/nbd.conf -a ! -e /etc/systemd/system/zaccel-swap.service ]
echo "  -- the Pi's case: nbd not loaded, modprobe fails, the prebuilt module is checked, installed and loaded"
rmmod nbd; unloaded=$?          # nothing uses nbd between phase D and here
echo "  nbd unloaded for this step: $([ $unloaded = 0 ] && echo yes || echo NO)"
: > "$T/fakebin/log"
out=$(unshare -m --propagation private bash "$T/ns_run.sh" "$T/pi" 1 2>&1); rc=$?
sed 's/^/  /' <<< "$out"
sed 's/^/  modprobe log: /' "$T/fakebin/log"
expect "install_pi.sh exits 0 with the Zynq down" [ $rc = 0 ]
grep -q "FATAL" "$T/fakebin/log"; failed_first=$?
grep -q "insmod .*/lib/modules/$K/updates/nbd.ko" "$T/fakebin/log"; from_updates=$?
grep -q "updates holds: 'nbd.ko '" <<< "$out"; placed=$?
expect "modprobe failed first, then the checked module went to updates/, depmod ran, and modprobe loaded it from there" \
  [ $unloaded = 0 -a $failed_first = 0 -a $placed = 0 -a $from_updates = 0 -a -d /sys/module/nbd ]
expect "modules-load.d/nbd.conf says nbd" [ "$(cat /etc/modules-load.d/nbd.conf 2>/dev/null)" = nbd ]
expect "WSL's real module tree untouched (no updates/, modules.* index unchanged)" \
  [ ! -e "/lib/modules/$K/updates" -a "$(modsums)" = "$MODSUMS_BEFORE" ]
systemd-analyze verify /etc/systemd/system/zaccel-swap.service /etc/systemd/system/zaccel-swap-retry.service \
  /etc/systemd/system/zaccel-swap-retry.timer /etc/systemd/system/zynqram-prep.service > "$T/verify.log" 2>&1; rc=$?
sed 's/^/  verify: /' "$T/verify.log"
expect "systemd-analyze verify accepts the Pi units and zynqram-prep.service (exit 0)" [ $rc = 0 ]
echo "  zaccel-swap.service $(systemctl is-active zaccel-swap.service)/$(systemctl is-enabled zaccel-swap.service), timer $(systemctl is-active zaccel-swap-retry.timer), module nbd in modules-load.d: $(cat /etc/modules-load.d/nbd.conf)"
journalctl -u zaccel-swap.service --no-pager -n 2 -o cat | sed 's/^/  journal: /'
expect "service active + enabled, retry timer running, no swap yet (Zynq absent)" \
  bash -c "[ \$(systemctl is-active zaccel-swap.service) = active ] && [ \$(systemctl is-enabled zaccel-swap.service) = enabled ] && [ \$(systemctl is-active zaccel-swap-retry.timer) = active ] && [ -z \"\$(awk 'NR>1 && \$1 ~ /nbd/' /proc/swaps)\" ]"
systemctl start nbd-server
t0=$(now)
for _ in $(seq 1 120); do [ -n "$(nbd_swaps)" ] && break; sleep 0.5; done
el=$(since "$t0")
journalctl -u zaccel-swap-retry.service --no-pager -n 2 -o cat | sed 's/^/  journal: /'
E1=$(cur_dev)
expect "Zynq came up: the retry timer attached $E1 at priority 100 within $el s" [ -n "$E1" -a "$(swap_prio "${E1:-x}")" = 100 ]
bash "$HERE/install_pi.sh" > "$T/pi2.log" 2>&1; rc=$?
expect "install_pi.sh again: exit 0, same single swap device" [ $rc = 0 -a "$(nbd_swaps | wc -l)" = 1 -a "$(cur_dev)" = "$E1" ]
systemctl stop zaccel-swap.service
sleep 0.5
echo "  after systemctl stop: service $(systemctl is-active zaccel-swap.service), timer $(systemctl is-active zaccel-swap-retry.timer), nbd swaps: '$(nbd_swaps)'"
expect "systemctl stop zaccel-swap: swap off, detached, retry timer stopped with it" \
  bash -c "[ -z \"\$(awk 'NR>1 && \$1 ~ /nbd/' /proc/swaps)\" ] && [ ! -e /run/zaccel-swap/device ] && [ \$(systemctl is-active zaccel-swap-retry.timer) = inactive ]"

# ---------------------------------------------------------------- F. put WSL back
hdr "F. cleanup: WSL back to how it was (packages stay)"
cleanup
left=""
for f in $OURS_ZYNQ $OURS_PI /run/zynqram /run/zaccel-swap /etc/systemd/system/zaccel-swap.service.d "$T" \
  "/lib/modules/$(uname -r)/updates"; do [ -e "$f" -o -L "$f" ] && left="$left $f"; done
echo "  /proc/swaps after:"; sed 's/^/    /' /proc/swaps
echo "  nbd module loaded: $([ -d /sys/module/nbd ] && echo yes || echo no); nbd-server processes: $(pgrep -c -x nbd-server); nbd-server.service: $(systemctl is-active nbd-server)"
expect "/proc/swaps is exactly as before" [ "$(cat /proc/swaps)" = "$SWAPS_BEFORE" ]
expect "no files, mounts or state left behind${left:+ (left:$left)}" [ -z "$left" ]
expect "nbd module back to its prior state, no nbd processes, nbd-server.service back to '$NBDSRV_WAS'" \
  bash -c "[ \$([ -d /sys/module/nbd ] && echo 1 || echo 0) = $NBD_WAS_LOADED ] && ! pgrep -x nbd-server >/dev/null && ! pgrep -x nbd-client >/dev/null && [ \$(systemctl is-active nbd-server) = '$NBDSRV_WAS' ]"

hdr "summary: $PASS passed, $FAIL failed"
printf '  %s\n' "${RESULTS[@]}"
[ "$FAIL" = 0 ]
