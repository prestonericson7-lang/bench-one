#!/bin/bash
# test_pl_detect.sh -- does zaccel-server decide "PL configured" the way the board needs?
# Linux's zynq-fpga driver clears devcfg PCFG_DONE when it probes, so on the booted board the bit is
# 0 even though U-Boot loaded pl.bit; boot.scr leaves fpgagpu.pl_loaded=1 on the command line instead.
# ZACCEL_DEVCFG_STS stands in for the register, ZACCEL_CMDLINE for /proc/cmdline. "Configured" shows as
# the server going on to look for its UIO windows (absent here) instead of stopping at the PL check.
# Usage: test_pl_detect.sh <server binary> [wrapper, e.g. qemu-arm-static]
set -u
BIN=$1; WRAP=${2:-}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
PASS=0; FAIL=0
case_() {  # case_ <name> <INT_STS> <cmdline> <expect: configured|not|reset>
  printf '%s\n' "$3" > "$W/cmdline"
  ZACCEL_DEVCFG_STS=$2 ZACCEL_CMDLINE=$W/cmdline timeout 5 $WRAP "$BIN" --port $((20000 + RANDOM % 20000)) > "$W/log" 2>&1 &
  local pid=$! i
  for i in $(seq 50); do grep -q "listening on TCP" "$W/log" && break; sleep 0.1; done
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  local line; line=$(grep -m1 "pl engine unavailable" "$W/log")
  local got=configured
  case $line in *"PL not configured"*) got=not ;; *"PL was reset after U-Boot"*) got=reset ;; esac
  [ -n "$line" ] || got="no pl line"
  if [ "$got" = "$4" ]; then echo "  PASS  $1"; PASS=$((PASS + 1)); else echo "  FAIL  $1: expected $4, got $got -- $line"; FAIL=$((FAIL + 1)); fi
}
M=fpgagpu.pl_loaded=1
B="console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 uio_pdrv_genirq.of_id=generic-uio"
case_ "PCFG_DONE set (before Linux's driver probes)"          0x00000004 "$B"                configured
case_ "PCFG_DONE cleared by Linux, marker present: the board" 0x00000000 "$B $M"             configured
case_ "marker at the start of the line"                       0x00000000 "$M $B"             configured
case_ "no pl.bit loaded: no marker, PCFG_DONE clear"          0x00000000 "$B"                not
case_ "PL reset after boot: marker, PCFG_INIT_NE set"         0x00000001 "$B $M"             reset
case_ "not the marker: a longer word"                         0x00000000 "$B ${M}0"          not
case_ "not the marker: a prefix glued on"                     0x00000000 "$B x$M"            not
case_ "other INT_STS bits do not matter"                      0xF8F7F87A "$B $M"             configured
echo "pl detect ($(basename "$BIN")): $PASS passed, $FAIL failed"
[ $FAIL = 0 ]
