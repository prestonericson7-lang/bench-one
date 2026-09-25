#!/usr/bin/env python3
"""
zynq_agent.py -- runs ON the PZ7020-StarLite (Debian rootfs built by hardware/pz7020-starlite/linux/).

It is the Zynq's voice in the car system: a TCP line server the Orange Pi's hub connects to
(hub.py --zynq <ip>:8091) and reads "KEY=value" lines from, exactly like the serial nodes.
No dependencies beyond the standard library, because the rootfs is minbase Debian.

What it publishes, once a second:
    ZYNQ_TEMP   die temperature from the PS XADC (iio in_temp0_raw/offset/scale), degC
    ZYNQ_UP     uptime, s
    ZYNQ_LOAD   1-minute load average
    PL_STATE    fpga_manager state ("operating" once a bitstream is loaded, else "unknown"/"power off")
    PL_BIT      name of the last firmware loaded through the fpga_manager (from /sys), or "-"
    ZYNQ_MEM    free memory, MB   (512 MB board: this number matters)

Every value is read from sysfs/procfs each tick -- nothing is cached, so a stale link is visible
as a stale age in the hub, never as a frozen number.

    python3 zynq_agent.py                # serve on 0.0.0.0:8091
    python3 zynq_agent.py --selftest     # fake sysfs, one client, checks every key parses
"""
import argparse
import re
import os
import socket
import socketserver
import sys
import threading
import time

PORT = 8091
IIO_GLOB = "/sys/bus/iio/devices"
FPGA_MGR = "/sys/class/fpga_manager/fpga0"


# ============================================================
#  readers  (pure functions over a root path -- unit testable)
# ============================================================
def read_first(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def xadc_temp(root="/"):
    """degC from the Zynq XADC via IIO: (raw + offset) * scale / 1000."""
    base = os.path.join(root, IIO_GLOB.lstrip("/"))
    try:
        devs = sorted(os.listdir(base))
    except OSError:
        return None
    for d in devs:
        p = os.path.join(base, d)
        raw = read_first(os.path.join(p, "in_temp0_raw"))
        if raw is None:
            continue
        off = float(read_first(os.path.join(p, "in_temp0_offset"), "0"))
        scale = float(read_first(os.path.join(p, "in_temp0_scale"), "1"))
        return round((float(raw) + off) * scale / 1000.0, 1)
    return None


def snapshot(root="/"):
    vals = {}
    t = xadc_temp(root)
    if t is not None:
        vals["ZYNQ_TEMP"] = t
    up = read_first(os.path.join(root, "proc/uptime"))
    if up:
        vals["ZYNQ_UP"] = int(float(up.split()[0]))
    la = read_first(os.path.join(root, "proc/loadavg"))
    if la:
        vals["ZYNQ_LOAD"] = float(la.split()[0])
    mem = read_first(os.path.join(root, "proc/meminfo"))
    if mem:
        for line in mem.splitlines():
            if line.startswith("MemAvailable:"):
                vals["ZYNQ_MEM"] = int(line.split()[1]) // 1024
    mgr = os.path.join(root, FPGA_MGR.lstrip("/"))
    vals["PL_STATE"] = read_first(os.path.join(mgr, "state"), "absent")
    vals["PL_BIT"] = read_first(os.path.join(mgr, "firmware"), "-") or "-"
    # the PL register file (pz7020_ps7_top): only believed when its ID answers
    pl = _pl_regs(root)
    if pl is not None:
        vals["PL_ID"] = f"{pl.rd(0x00):#010x}"
        vals["PL_TIME"] = round(pl.time_s(), 3)
        vals["FAN_RPM"] = pl.rd(0x18)
        vals["FAN_DUTY"] = pl.rd(0x14)
        vals["PL_KEY"] = pl.rd(0x10)
    return vals


_PL = {"obj": None, "tried": False}


def _pl_regs(root):
    """Open /dev/mem once (real board) or use an injected fake (selftest); None if not present."""
    if root != "/":
        return _PL["obj"]                      # selftest injects a PlRegs over a FakeWindow
    # re-check every 10 s while absent: a bitstream can be loaded later through the fpga_manager
    if _PL["obj"] is None and time.monotonic() >= _PL.get("next", 0):
        _PL["next"] = time.monotonic() + 10
        try:
            if "/usr/local/bin" not in sys.path:
                sys.path.append("/usr/local/bin")   # pl_regs.py is installed beside the agent
            import pl_regs
            r = pl_regs.PlRegs.open()          # refuses (no bus error) unless PCFG_DONE is set
            _PL["obj"] = r if r.present() else None
        except Exception:                      # noqa: BLE001 -- no /dev/mem rights, no bitstream: report nothing
            _PL["obj"] = None
    return _PL["obj"]


def format_lines(vals):
    out = []
    for k, v in vals.items():
        out.append(f"{k}={v:.1f}" if isinstance(v, float) else f"{k}={v}")
    return out


# ============================================================
#  server
# ============================================================
class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        root = self.server.root
        period = self.server.period
        try:
            while True:
                for ln in format_lines(snapshot(root)):
                    self.wfile.write((ln + "\n").encode())
                self.wfile.flush()
                time.sleep(period)
        except (BrokenPipeError, ConnectionResetError, OSError):
            return


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, root="/", period=1.0):
        super().__init__(addr, Handler)
        self.root, self.period = root, period


# ============================================================
#  selftest
# ============================================================
def selftest():
    import tempfile
    ok = True
    root = tempfile.mkdtemp()
    # fake sysfs: XADC raw 2600 with offset 0 scale 123.04 -> 319.9 -> well, use realistic values
    # real name is "iio:device0"; a colon is illegal in a Windows path and the reader accepts any dir name
    iio = os.path.join(root, "sys/bus/iio/devices/iio_device0"); os.makedirs(iio)
    open(os.path.join(iio, "in_temp0_raw"), "w").write("2600\n")
    open(os.path.join(iio, "in_temp0_offset"), "w").write("-2219\n")
    open(os.path.join(iio, "in_temp0_scale"), "w").write("123.040771484\n")
    mgr = os.path.join(root, "sys/class/fpga_manager/fpga0"); os.makedirs(mgr)
    open(os.path.join(mgr, "state"), "w").write("operating\n")
    open(os.path.join(mgr, "firmware"), "w").write("fan_top.bin\n")
    os.makedirs(os.path.join(root, "proc"))
    open(os.path.join(root, "proc/uptime"), "w").write("1234.56 4000.00\n")
    open(os.path.join(root, "proc/loadavg"), "w").write("0.42 0.30 0.20 1/80 900\n")
    open(os.path.join(root, "proc/meminfo"), "w").write("MemTotal: 500000 kB\nMemAvailable: 409600 kB\n")

    import pl_regs as plr
    fw = plr.FakeWindow(); fw.tick(); _PL["obj"] = plr.PlRegs(fw)

    print("1) readers against a fake sysfs (+ a fake PL register window)")
    v = snapshot(root)
    want = {"ZYNQ_TEMP": 46.9, "ZYNQ_UP": 1234, "ZYNQ_LOAD": 0.42, "ZYNQ_MEM": 400,
            "PL_STATE": "operating", "PL_BIT": "fan_top.bin", "PL_ID": "0x5a702001",
            "FAN_DUTY": 60, "FAN_RPM": 0, "PL_KEY": 0}
    if not isinstance(v.get("PL_TIME"), float) or v["PL_TIME"] <= 0:
        print(f"   FAIL PL_TIME: {v.get('PL_TIME')!r}"); ok = False
    for k, w in want.items():
        got = v.get(k)
        good = (abs(got - w) < 0.05) if isinstance(w, float) else (got == w)
        if not good:
            print(f"   FAIL {k}: got {got!r}, want {w!r}"); ok = False
    if ok:
        print(f"   ok: {v}")

    print("2) one client over TCP gets every key as KEY=value lines")
    srv = Server(("127.0.0.1", 0), root=root, period=0.05)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    with socket.create_connection(srv.server_address, timeout=2) as s:
        data = b""
        while data.count(b"\n") < 2 * len(want):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    srv.shutdown()
    lines = [l for l in data.decode().split("\n") if l]
    keys = {l.split("=")[0] for l in lines}
    if not set(want) <= keys:
        print(f"   FAIL: missing keys {set(want) - keys}"); ok = False
    else:
        print(f"   ok: {len(lines)} lines, keys {sorted(keys)}")
    bad = [l for l in lines if "=" not in l]
    if bad:
        print(f"   FAIL: malformed lines {bad}"); ok = False

    print("3) missing sysfs degrades to absent values, never a crash")
    _PL["obj"] = None
    v2 = snapshot(tempfile.mkdtemp())
    if v2.get("PL_STATE") != "absent" or "ZYNQ_TEMP" in v2:
        print(f"   FAIL: {v2}"); ok = False
    else:
        print(f"   ok: {v2}")

    print("4) beacon: the hub can find this board without knowing its address")
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); rx.bind(("127.0.0.1", 0)); rx.settimeout(3)
    b = Beacon(PORT, targets=[("127.0.0.1", rx.getsockname()[1])], period=0.1)
    b.start()
    try:
        data, _ = rx.recvfrom(256)
        parts = data.decode().split()
        if parts[:2] != ["ZYNQ-AGENT", str(PORT)]:
            print(f"   FAIL: {data!r}"); ok = False
        else:
            print(f"   ok: {data.decode()!r}")
    except socket.timeout:
        print("   FAIL: no beacon"); ok = False
    b.stop_flag = True; rx.close()
    bc = broadcast_targets("3: eth0    inet 192.168.2.77/24 brd 192.168.2.255 scope global eth0\n"
                           "3: eth0    inet 10.20.0.2/24 brd 10.20.0.255 scope global eth0\n"
                           "1: lo    inet 127.0.0.1/8 scope host lo\n")
    if bc != ["192.168.2.255", "10.20.0.255", "255.255.255.255"]:
        print(f"   FAIL: broadcast targets {bc}"); ok = False
    else:
        print(f"   ok: one directed broadcast per address: {bc}")

    print("\nPASSED" if ok else "\nFAILED")
    return ok


# ============================================================
#  beacon
# ============================================================
BEACON_PORT = 8092


def broadcast_targets(ip_output=None):
    """Directed broadcast address of every IPv4 address ('ip -o -4 addr'), then the global one.

    A plain 255.255.255.255 leaves only via the default route -- and the car LAN has none -- so each
    interface address gets its own directed broadcast (10.20.0.255 on the car LAN, the home LAN's
    on the bench)."""
    if ip_output is None:
        try:
            import subprocess
            ip_output = subprocess.run(["ip", "-o", "-4", "addr", "show"], capture_output=True,
                                       text=True, timeout=5).stdout
        except Exception:                      # noqa: BLE001 - no iproute2 (tests on Windows)
            ip_output = ""
    out = [m for m in re.findall(r"\bbrd (\d+\.\d+\.\d+\.\d+)", ip_output)]
    return list(dict.fromkeys(out)) + ["255.255.255.255"]


class Beacon(threading.Thread):
    """Announces 'ZYNQ-AGENT <tcp port> <hostname>' on UDP 8092 every `period` seconds."""

    daemon = True

    def __init__(self, tcp_port, targets=None, period=2.0):
        super().__init__(name="beacon")
        self.msg = f"ZYNQ-AGENT {tcp_port} {socket.gethostname()}".encode()
        self.targets, self.period = targets, period
        self.stop_flag = False

    def run(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        while not self.stop_flag:
            dests = self.targets or [(a, BEACON_PORT) for a in broadcast_targets()]
            for d in dests:
                try:
                    s.sendto(self.msg, d)
                except OSError:
                    pass                           # an interface without a route: try the others
            time.sleep(self.period)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--period", type=float, default=1.0)
    ap.add_argument("--no-beacon", action="store_true")
    ap.add_argument("--beacon-to", action="append", default=None,
                    help="host:port to send the beacon to instead of the broadcasts (tests)")
    ap.add_argument("--root", default="/", help="filesystem root for sysfs/procfs (tests)")
    ap.add_argument("--bind", default="0.0.0.0")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if not a.no_beacon:
        tg = [(h, int(p)) for h, _, p in (x.partition(":") for x in a.beacon_to)] if a.beacon_to else None
        Beacon(a.port, targets=tg).start()
    srv = Server((a.bind, a.port), root=a.root, period=a.period)
    print(f"zynq_agent: serving KEY=value lines on :{a.port}, beacon on UDP {BEACON_PORT}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
