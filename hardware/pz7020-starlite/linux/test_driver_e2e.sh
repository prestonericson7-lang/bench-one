#!/bin/bash
# test_driver_e2e.sh -- board_experiment.py end to end against emulated boards (QEMU in WSL, console on TCP),
# every path it can take short of real silicon. Run from Git Bash on the PC, after qemu_plcheck_test.sh (it
# leaves /root/zynq/plcheck-test/{A,B}.img in WSL):
#     bash hardware/pz7020-starlite/linux/test_driver_e2e.sh
#   1 fix failed: card #1's boot.scr and device tree (Linux gates FCLK0), a noisy fake CH340 beside it. The
#     board's port is picked, the gate found and cleared, every step runs, the reboot is captured.
#   2 stuck: today's files, the guard's PL read made to never return (mk_stuck_guard_img.sh). One Enter after
#     the login finds the shell, the stuck read is named, the PL is not read again.
#   3 already up: the board booted before the PC listened. One Enter after 90 s of silence finds the prompt.
#   4 frozen boot: QEMU stopped once systemd runs: SILENT with the last lines, exit 3.
#   5 no return: the board never comes back after the stall test: the verdicts so far, exit 3.
#   6 refused: no U-Boot (level shifters off, fabric in reset): the guard's other problems stop the PL steps.
#   7 the Pi check: the driver's own command that finds the Orange Pi attached to the board's swap export
#     (then the stall test is skipped), on the emulated board's nbd-server, with and without a client.
#   8 no guard log: the guard masked (systemd.mask=), no U-Boot: the driver's own register reading finds the
#     level shifters off and the fabric in reset, and refuses the PL by itself.
# Captures in captures/driver-e2e-<time>/. Not covered here: real COM ports (the 10-minute "nothing heard"
# message only fires for a port that appears after the start) and the message boxes (tell_owner skips
# socket ports).
set -u
L=$(cd "$(dirname "$0")" && pwd)
LW=/mnt$L                                  # the same directory, seen from WSL
O=$L/captures/driver-e2e-$(date +%Y%m%d-%H%M%S); mkdir -p "$O"
fail=0; ok() { echo "  PASS  $*"; }; bad() { echo "  FAIL  $*"; fail=1; }
w() { MSYS_NO_PATHCONV=1 wsl -d Ubuntu-22.04 -u root --exec "$@"; }
# the kernel booted directly (no U-Boot: level shifters off, fabric in reset); EXTRA: more kernel arguments
qtcp() { w env TIMEOUT=2400 PORT="$1" EXTRA="${2:-}" bash "$LW/qemu_serial_tcp.sh" > "$O/qemu-$1.out" 2>&1 & }
# the production U-Boot at the board's clocks, from an image
qub() { w env SERIAL_TCP="$1" IMG="$2" TIMEOUT=2400 bash "$LW/qemu_uboot_test.sh" > "$O/qemu-$1.out" 2>&1 & }
qkill() { w bash "$LW/qemu_kill_port.sh" "$1" "$2"; }
drv() { local n=$1; shift; (cd "$L" && python board_experiment.py "$@" > "$O/driver-$n.out" 2>&1; echo "driver rc=$?" >> "$O/driver-$n.out"); }
has() { grep -q -- "$2" "$O/driver-$1.out"; }
want() { if has "$1" "$2"; then ok "$1: $3"; else bad "$1: $3 (no '$2')"; fi; }
wantnot() { if has "$1" "$2"; then bad "$1: $3 ('$2' found)"; else ok "$1: $3"; fi; }
waitfor() { local end=$((SECONDS + $3)); until has "$1" "$2"; do [ $SECONDS -gt $end ] && return 1; sleep 1; done; }

# a Windows checkout may hand WSL these scripts with CRLF line ends
w sed -i 's/\r$//' "$LW/qemu_serial_tcp.sh" "$LW/qemu_uboot_test.sh" "$LW/qemu_kill_port.sh" "$LW/mk_stuck_guard_img.sh"
echo "== building the stuck-guard image"
w bash "$LW/mk_stuck_guard_img.sh" | tail -1

echo "== 1 fix failed and 2 stuck guard (in parallel)"
qub 5571 /root/zynq/plcheck-test/A.img
qub 5573 /root/zynq/stuck-guard.img
python "$L/fake_ch340.py" 5572 > "$O/fake-5572.out" 2>&1 &
sleep 5
drv 1 --port socket://localhost:5572,socket://localhost:5571 --test-reboot &
D1=$!
drv 2 --port socket://localhost:5573 &
D2=$!
wait $D1 $D2
qkill KILL 5571; qkill KILL 5573
want 1 "the board's console is socket://localhost:5571" "the board's port picked"
want 1 "socket://localhost:5572 is not the board -- closed" "the fake CH340 left alone"
want 1 "FCLK0 gate bit SET" "the gate Linux set is seen"
want 1 "step 2: the guard found FCLK0 gated" "the gate cleared before the read"
wantnot 1 "has NOT returned" "the PL read came back once the clock ran"
want 1 "step 4: the gate test" "the gate test ran"
want 1 "the board is back up on the same card" "the reboot captured"
want 1 "step 6: restore" "restored"
want 1 "PLX SLCR_LOCKSTA 0" "the SLCR left unlocked, as Linux needs"
want 1 "| VERDICT FPGA0_THR_STA" "the record on the card, read back after the base64 transfer"
want 1 "driver rc=0" "exit 0"
want 2 "logged in 60 s ago and no report" "the missing report noticed"
want 2 "the shell answers" "the shell found with one Enter"
want 2 "PLX guard read stuck: 0x40000000 (platform registers)" "the stuck read named"
want 2 "the PL is NOT read again" "the PL left alone"
want 2 "VERDICT boot: the guard's read of 0x40000000" "a verdict for it"
wantnot 2 "step 3:" "no measurement attempted"
wantnot 2 "PLX rd 0x" "no PL read by the driver"
want 2 "driver rc=0" "exit 0"
c=$(cat "$L"/captures/experiment-*/console-socket_localhost_5573.log 2>/dev/null | grep -a -c "A start job is running")
echo "   (2: systemd 'A start job is running' lines on that console: $c -- 0 in QEMU on 2026-10-06; the driver does not rely on them)"

echo "== 3 already up, 6 refused and 7 the Pi check (in parallel)"
qtcp 5574
qtcp 5577 fpgagpu.pl_loaded=1
qtcp 5578
sleep 3
# 7: the exact command the driver uses to see the Pi attached (PI_ATTACHED_CMD, read from its source), on an
# emulated board's real nbd-server: 0 with nothing connected, 1 with one TCP client on the export's port
python - "$(cygpath -m "$L")/board_experiment.py" > "$O/pi-check.out" 2>&1 <<'PY' &
import ast, re, sys, time, serial
cmd = ast.literal_eval(re.search(r'^PI_ATTACHED_CMD = (".*")$', open(sys.argv[1], encoding="utf-8").read(), re.M).group(1))
for _ in range(240):
    try:
        s = serial.serial_for_url("socket://localhost:5578", timeout=0.2); break
    except Exception:
        time.sleep(1)
b = b""; end = time.time() + 900
while time.time() < end and b"ZYNQ-REPORT END" not in b:
    b += s.read(4096)
n = [0]
def sh(line, limit=60):
    n[0] += 1; tag = "__T%dE__" % n[0]; s.write((line + "; echo '__T%d''E__' $?\r" % n[0]).encode())
    out, end = b"", time.time() + limit
    while time.time() < end and tag.encode() not in out:
        out += s.read(4096)
    t = re.sub(r"\x1b\[[?0-9;]*[A-Za-z]", "", out.decode("utf-8", "replace")).replace("\r", "")   # as the driver's text()
    return t.split("\n", 1)[1].split(tag)[0].strip() if tag in t and "\n" in t else None
time.sleep(3); sh("stty cols 4000")
print("PI-CHECK none:", (sh(cmd) or "?").splitlines()[-1:])
sh("python3 -c \"import socket,time;s=socket.create_connection(('127.0.0.1',10809));time.sleep(300)\" & sleep 3")
print("PI-CHECK one client:", (sh(cmd) or "?").splitlines()[-1:])
print("PI-CHECK nbd-server:", (sh("systemctl is-active nbd-server") or "?").splitlines()[-1:])
try:
    s.close()
except Exception:
    pass
PY
P7=$!
python - <<'PY'
import time, serial
for _ in range(240):
    try:
        s = serial.serial_for_url("socket://localhost:5574", timeout=0.2); break
    except Exception:
        time.sleep(1)
b = b""; end = time.time() + 900
while time.time() < end and b"ZYNQ-REPORT END" not in b:
    b += s.read(4096)
time.sleep(5); s.close()
print("   3: a throwaway client saw the boot and disconnected")
PY
drv 3 --port socket://localhost:5574 --no-stall --ignore-guard &
D3=$!
drv 6 --port socket://localhost:5577 &
D6=$!
wait $D3 $D6 $P7
qkill KILL 5574; qkill KILL 5577; qkill KILL 5578
sed 's/^/   7: /' "$O/pi-check.out"
grep -q "PI-CHECK nbd-server: \['active'\]" "$O/pi-check.out" && ok "7: the emulated board's nbd-server runs" || bad "7: nbd-server not active"
grep -q "PI-CHECK none: \['0'\]" "$O/pi-check.out" && ok "7: no client -> 0 (the stall test runs)" || bad "7: no-client count"
grep -q "PI-CHECK one client: \['1'\]" "$O/pi-check.out" && ok "7: one client on the export -> 1 (the stall test is skipped)" || bad "7: one-client count"
want 3 "sent one Enter in case Linux is already up" "one Enter after 90 s of silence"
want 3 "already up when the PC began listening" "the board found already up"
want 3 "driver rc=0" "exit 0"
want 6 "a problem other than FCLK0 was found -- the PL is NOT read" "refused on the guard's other problems"
wantnot 6 "problem (driver)" "the driver's own check adds nothing the guard already said"
want 6 "steps 2-5 skipped" "steps skipped"
want 6 "| steps 2-5 skipped" "the skip recorded on the card (read back)"
wantnot 6 "PLX rd 0x" "no PL read"
want 6 "driver rc=0" "exit 0"

echo "== 4 frozen boot, 5 no return and 8 no guard log (in parallel)"
qtcp 5575
qtcp 5576 fpgagpu.pl_loaded=1
qtcp 5579 "fpgagpu.pl_loaded=1 systemd.mask=zynq-plcheck.service"
sleep 3
drv 4 --port socket://localhost:5575 --ignore-guard &
D4=$!
(export BOARD_EXP_REBOOT_WAIT=60; drv 5 --port socket://localhost:5576 --ignore-guard --test-reboot) &
D5=$!
drv 8 --port socket://localhost:5579 &
D8=$!
# each emulator stopped by its own watcher, so neither waits on the other's timing
(waitfor 4 "boot: systemd" 900 && { sleep 5; qkill STOP 5575; echo "   4: QEMU stopped (SIGSTOP) once systemd ran"; }) &
W4=$!
(waitfor 5 "step 5 replaced" 1500 && { qkill KILL 5576; echo "   5: QEMU killed as the reboot was sent"; }) &
W5=$!
wait $D4 $D5 $W4 $W5 $D8
qkill KILL 5575; qkill KILL 5579
want 4 "SILENT for" "the stop noticed"
want 4 "driver rc=3" "exit 3"
want 5 "serial read failed" "the lost console noticed"
want 5 "VERDICT FCLK0" "the verdicts of steps 3-4 kept"
want 5 "driver rc=3" "exit 3"
want 8 "no guard log for this boot" "the missing guard log noticed"
want 8 "problem (driver): PS/PL level shifters not all on" "the driver's own reading finds the level shifters off"
want 8 "problem (driver): fabric reset 0 asserted" "and the fabric in reset"
want 8 "a problem other than FCLK0 was found -- the PL is NOT read" "refused without the guard"
wantnot 8 "PLX rd 0x" "no PL read"
want 8 "driver rc=0" "exit 0"

echo "captures: $O"
[ $fail = 0 ] && echo "DRIVER E2E: PASS" || echo "DRIVER E2E: FAIL"
exit $fail
