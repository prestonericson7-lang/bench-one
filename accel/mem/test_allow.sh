#!/bin/bash
# test_allow.sh -- the shipped zynqram export config admits the Pi's links and refuses anyone else.
# Runs the real nbd-server on the shipped zynqram.conf + zynqram.allow (only the backing file and the
# port changed), then attaches from an allowed and from a refused address. WSL, root. Cleans up.
set -u
H=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); PORT=10899; rc=0
truncate -s 64M "$T/zynqram.img"
# listenaddr as install_zynq.sh sets it on the Zynq (IPv4 only; see there for the nbd-server bug)
printf '[generic]\n\tport = %s\n\tlistenaddr = 0.0.0.0\n\tallowlist = false\n' $PORT > "$T/config"
sed -e "s#/run/zynqram/zynqram.img#$T/zynqram.img#" -e "s#/etc/nbd-server/zynqram.allow#$T/zynqram.allow#" \
    "$H/zynq/zynqram.conf" >> "$T/config"
cp "$H/zynq/zynqram.allow" "$T/zynqram.allow"
ip addr add 10.99.0.1/32 dev lo 2>/dev/null; al1=$?
ip addr add 10.20.0.2/32 dev lo 2>/dev/null; al2=$?
nbd-server -C "$T/config" 2>"$T/srv.log"; sleep 1
modprobe nbd 2>/dev/null
try() {  # try <server address> -> 0 when attached
  nbd-client -d /dev/nbd6 >/dev/null 2>&1
  timeout 20 nbd-client "$1" $PORT /dev/nbd6 -N zynqram >"$T/c.log" 2>&1; local r=$?
  [ $r = 0 ] && nbd-client -d /dev/nbd6 >/dev/null 2>&1
  return $r
}
try 127.0.0.1 && echo "PASS  127.0.0.1 (the board itself) attaches" || { echo "FAIL  127.0.0.1 refused: $(tail -1 $T/c.log)"; rc=1; }
try 10.20.0.2 && echo "PASS  a car-LAN address (10.20.0.x) attaches" || { echo "FAIL  10.20.0.2 refused: $(tail -1 $T/c.log)"; rc=1; }
try 10.99.0.1 && { echo "FAIL  10.99.0.1 (a stranger) was allowed to attach"; rc=1; } || echo "PASS  10.99.0.1 (not on the Pi's links) is refused"
pkill -f "nbd-server -C $T/config"
[ $al1 = 0 ] && ip addr del 10.99.0.1/32 dev lo
[ $al2 = 0 ] && ip addr del 10.20.0.2/32 dev lo
rm -rf "$T"
exit $rc
