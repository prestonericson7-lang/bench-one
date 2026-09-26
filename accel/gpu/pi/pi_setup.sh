#!/usr/bin/env bash
# pi/pi_setup.sh -- one-time setup of the Orange Pi 4 Pro for the FPGA-GPU (SPEC 11). Idempotent:
# running it again changes nothing that is already set up.
#
#   sudo ./pi_setup.sh [--iface IF] [--no-net] [--no-install] [--dry-run]
#
#   1. wired interface -> 10.77.0.1/24 for the direct cable to the Zynq (10.77.0.2):
#      NetworkManager profile "fpgagpu" (manual address, never-default, autoconnect) if nmcli is
#      usable, else "ip addr add" (not persistent across reboots; the script says so).
#      Skipped when the repo's car LAN already owns the wired port (NetworkManager profile
#      "car-lan" from deploy/orangepi/install.sh, or 10.20.0.1 on the interface): the Zynq is
#      10.20.0.2 there, the daemon listens on every address, and the tools try 10.77.0.2 then
#      10.20.0.2 by themselves -- this script never takes the port away from car-lan.
#   2. $SUDO_USER -> group dialout (Teensy tty access; takes effect at the next login)
#   3. udev rule: ModemManager must not probe the Teensy's serial port (AT commands would land in
#      the geometry protocol)
#   4. tools -> /usr/local/bin (from build/aarch64/ of this directory, or build/native/, or this
#      directory itself; builds natively with "make native" if only the sources are here)
#   5. ping the Zynq (10.77.0.2, then 10.20.0.2) and print the next steps
# --iface IF picks the interface (default: the first wired Ethernet interface: /sys/class/net/*
# with a device, type 1, not wireless). --dry-run prints the commands instead of running them.
# The tools take the Zynq's address from --fpga HOST[:PORT] or FPGAGPU_HOST=HOST[:PORT].
set -u

ADDR=10.77.0.1
PREFIX=24
PEER=10.77.0.2
CARLAN_PROFILE=car-lan
CARLAN_ADDR=10.20.0.1
CARLAN_PEER=10.20.0.2
PROFILE=fpgagpu
BINDIR=/usr/local/bin
UDEV_RULE=/etc/udev/rules.d/49-fpgagpu-teensy.rules
TOOLS="gpu_selftest gpu_demo gpu_view gpu_stat gpu_snap gpu_image"

IFACE=""
DO_NET=1
DO_INSTALL=1
DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
    --iface) IFACE=${2:-}; shift ;;
    --no-net) DO_NET=0 ;;
    --no-install) DO_INSTALL=0 ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "pi_setup.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

run() {
    if [ "$DRY" = 1 ]; then
        echo "  [dry-run] $*"
        return 0
    fi
    "$@"
}
note() { echo "pi_setup: $*"; }
warn() { echo "pi_setup: WARNING: $*" >&2; }

if [ "$DRY" = 0 ] && [ "$(id -u)" != 0 ]; then
    echo "pi_setup.sh must run as root: sudo $0 $*" >&2
    exit 1
fi
HERE=$(cd "$(dirname "$0")" && pwd)
RC=0

# ---- 1. network ------------------------------------------------------------------------------
pick_iface() {
    local d n best=""
    for d in /sys/class/net/*; do
        n=$(basename "$d")
        [ "$n" = lo ] && continue
        [ -e "$d/device" ] || continue                  # virtual (bridge, docker, veth, ...)
        [ -d "$d/wireless" ] || [ -e "$d/phy80211" ] && continue
        [ "$(cat "$d/type" 2>/dev/null)" = 1 ] || continue
        case "$n" in
        e*) echo "$n"; return 0 ;;                     # eth0, end0, enp1s0, ...
        esac
        [ -z "$best" ] && best=$n
    done
    [ -n "$best" ] && echo "$best"
}

nm_usable() {
    command -v nmcli > /dev/null 2>&1 && nmcli -t -f RUNNING general 2> /dev/null | grep -q '^running$'
}

carlan_profile() {
    nm_usable && nmcli -t -f NAME connection show 2> /dev/null | grep -qx "$CARLAN_PROFILE"
}

if [ "$DO_NET" = 1 ]; then
    [ -n "$IFACE" ] || IFACE=$(pick_iface)
    if [ -z "$IFACE" ] || [ ! -e "/sys/class/net/$IFACE" ]; then
        warn "no wired Ethernet interface found (use --iface IF)"
        RC=1
    elif carlan_profile || ip -4 addr show dev "$IFACE" 2> /dev/null | grep -q "inet $CARLAN_ADDR/"; then
        note "the car LAN owns the wired port (NetworkManager profile '$CARLAN_PROFILE' or $CARLAN_ADDR on $IFACE):"
        note "leaving it alone. The Zynq is $CARLAN_PEER there; the tools try $PEER, then $CARLAN_PEER"
        note "(or set FPGAGPU_HOST=HOST / use --fpga HOST)."
        if nm_usable && nmcli -t -f NAME connection show 2> /dev/null | grep -qx "$PROFILE"; then
            note "an older '$PROFILE' profile exists; car-lan (autoconnect priority 100) wins over it (50)."
            note "Remove it if the cable only ever goes to the car LAN: sudo nmcli connection delete $PROFILE"
        fi
    elif nm_usable; then
        note "NetworkManager: profile '$PROFILE' on $IFACE -> $ADDR/$PREFIX (manual, never-default, autoconnect)"
        if nmcli -t -f NAME connection show 2> /dev/null | grep -qx "$PROFILE"; then
            run nmcli connection modify "$PROFILE" connection.interface-name "$IFACE" ipv4.method manual \
                ipv4.addresses "$ADDR/$PREFIX" ipv4.gateway "" ipv4.never-default yes ipv6.method ignore \
                connection.autoconnect yes connection.autoconnect-priority 50 || RC=1
        else
            run nmcli connection add type ethernet con-name "$PROFILE" ifname "$IFACE" ipv4.method manual \
                ipv4.addresses "$ADDR/$PREFIX" ipv4.never-default yes ipv6.method ignore \
                connection.autoconnect yes connection.autoconnect-priority 50 || RC=1
        fi
        if ! run nmcli connection up "$PROFILE" > /dev/null 2>&1; then
            warn "profile '$PROFILE' not active yet (cable unplugged or FPGA board off?); NetworkManager"
            warn "activates it by itself when the link comes up"
        fi
        note "the wired port $IFACE now belongs to the FPGA link (profile priority 50); use Wi-Fi for the"
        note "network. Undo: sudo nmcli connection delete $PROFILE"
    else
        note "no NetworkManager: adding $ADDR/$PREFIX to $IFACE with ip (NOT persistent across reboots)"
        run ip link set "$IFACE" up || RC=1
        if ip -4 addr show dev "$IFACE" 2> /dev/null | grep -q "inet $ADDR/"; then
            note "$IFACE already has $ADDR"
        else
            run ip addr add "$ADDR/$PREFIX" dev "$IFACE" || RC=1
        fi
        note "to make it permanent add it to your distribution's network configuration"
        note "(e.g. /etc/network/interfaces: 'iface $IFACE inet static' / 'address $ADDR/$PREFIX')"
    fi
fi

# ---- 2. dialout ------------------------------------------------------------------------------
U=${SUDO_USER:-}
if [ -n "$U" ] && [ "$U" != root ]; then
    if id -nG "$U" 2> /dev/null | tr ' ' '\n' | grep -qx dialout; then
        note "$U is already in the dialout group"
    else
        note "adding $U to the dialout group (log out and in again for it to take effect)"
        run usermod -aG dialout "$U" || RC=1
    fi
else
    note "run via sudo from your user account to add it to the dialout group (SUDO_USER is not set)"
fi

# ---- 3. udev: keep ModemManager away from the Teensy -----------------------------------------------
RULE='# FPGA-GPU (pi/pi_setup.sh): ModemManager must not probe the Teensy geometry engine (PJRC VID 16c0).
ATTRS{idVendor}=="16c0", ATTRS{idProduct}=="04*", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{ID_MM_PORT_IGNORE}="1"'
if [ -f "$UDEV_RULE" ] && [ "$(cat "$UDEV_RULE")" = "$RULE" ]; then
    note "udev rule $UDEV_RULE already installed"
elif [ -d /etc/udev/rules.d ]; then
    note "installing $UDEV_RULE"
    if [ "$DRY" = 1 ]; then
        echo "  [dry-run] write $UDEV_RULE"
    else
        printf '%s\n' "$RULE" > "$UDEV_RULE" || RC=1
        udevadm control --reload-rules > /dev/null 2>&1 || true
    fi
fi

# ---- 4. tools ------------------------------------------------------------------------------------
if [ "$DO_INSTALL" = 1 ]; then
    SRC=""
    for d in "$HERE/build/aarch64" "$HERE/build/native" "$HERE"; do
        if [ -x "$d/gpu_selftest" ] && "$d/gpu_selftest" --offline > /dev/null 2>&1; then SRC=$d; break; fi
    done
    if [ -z "$SRC" ] && [ -f "$HERE/Makefile" ] && command -v make > /dev/null && command -v cc > /dev/null; then
        note "no runnable binaries here: building natively (make native)"
        if [ -n "$U" ] && [ "$U" != root ]; then
            run sudo -u "$U" make -C "$HERE" native > /dev/null || RC=1
        else
            run make -C "$HERE" native > /dev/null || RC=1
        fi
        [ -x "$HERE/build/native/gpu_selftest" ] && SRC=$HERE/build/native
    fi
    if [ -z "$SRC" ]; then
        warn "no runnable gpu_* binaries found (build them on the PC: make -C pi pi, then copy pi/ here)"
        RC=1
    else
        note "installing $TOOLS from $SRC to $BINDIR"
        for t in $TOOLS; do
            if [ -x "$SRC/$t" ]; then run install -m 0755 "$SRC/$t" "$BINDIR/$t" || RC=1; else warn "$SRC/$t missing"; RC=1; fi
        done
    fi
fi

# ---- 5. reachability + next steps ------------------------------------------------------------------
if [ "$DO_NET" = 1 ] && [ "$DRY" = 1 ]; then
    echo "  [dry-run] ping -c 3 -W 1 $PEER || ping -c 3 -W 1 $CARLAN_PEER"
elif [ "$DO_NET" = 1 ]; then
    FOUND=""
    for p in "$PEER" "$CARLAN_PEER"; do
        if ping -c 3 -W 1 "$p" > /dev/null 2>&1; then FOUND=$p; break; fi
    done
    if [ -n "$FOUND" ]; then
        note "$FOUND answers ping: the Zynq board is reachable"
    else
        warn "neither $PEER nor $CARLAN_PEER answers ping (board off / still booting / cable / its eth0"
        warn "address): check 'ip -4 addr show ${IFACE:-<iface>}' here and 'ip -4 addr show eth0' on the Zynq"
    fi
fi
cat << EOF

Next steps
  1. log out and in again (dialout group), plug in the Teensy (USB) and the Zynq board (Ethernet)
     (the tools find the Zynq at $PEER or $CARLAN_PEER; elsewhere: FPGAGPU_HOST=HOST or --fpga HOST)
  2. gpu_stat                        daemon + PL registers (expect ID 'GPU1' ok, MMCM_LOCKED)
     gpu_stat --teensy auto          ... plus the Teensy (fpga_ready 1, bus_enabled 1)
  3. gpu_selftest                    bit-exact end-to-end test of every path (exit 0 = all PASS)
  4. gpu_demo                        3D demo on the HDMI screen (Ctrl-C to stop)
     gpu_demo --view                 ... and the returned frames in a window on this desktop
     gpu_demo --auto --view          Teensy autonomous mode
  5. gpu_view                        watch whatever the FPGA renders
     gpu_snap -o shot.png            screenshot of the HDMI output
     gpu_image picture.ppm           show a picture full screen
EOF
exit $RC
