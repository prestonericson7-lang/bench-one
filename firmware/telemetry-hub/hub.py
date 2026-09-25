#!/usr/bin/env python3
"""
hub.py -- the telemetry hub. Runs on the Orange Pi; it is what makes the fleet
a system rather than a pile of boards.

    CAN logger (Teensy, USB)  ┐                     ┌─>  vent display (Teensy)  KEY=value lines
    climate node (ESP32, USB) ├─>  hub  (Store)  ───┼─>  HTTP /api /health /devices
    Zynq (Ethernet, :8091)    ┘                     └─>  voice assistant (current fix)

FINDS EVERYTHING BY ITSELF
  * Serial nodes: every USB serial port from a known vendor (Teensy 16C0, Espressif 303A, and the
    common USB-UART bridges) is asked "ID?". The firmware answers ID=car-can-logger,
    ID=vent-display or ID=climate-node, and the port is handed to the matching worker. Two Teensys
    share one VID:PID, so the answer -- not the port name -- decides who is who. Unplug and replug
    is handled: a worker that loses its port releases it and the next scan picks it up again.
  * The Zynq: zynq_agent.py broadcasts "ZYNQ-AGENT <port> <host>" on UDP 8092 every 2 s. The hub
    connects to whoever announces; if nothing announces it also tries the fixed car-LAN address
    10.20.0.2:8091.

DESIGN RULES CARRIED FROM THE ARCHITECTURE
  * Every value carries an AGE. A stale number presented as live is worse than no number; the
    vent display is only ever sent fresh values and shows LINK when they stop.
  * Nothing here may block. A dead source must not stall the others; every source is its own
    thread, reconnects on its own, and reports its state as a fault until it is healthy.
  * Machine lines are KEY=value. Lines starting with '#' are for humans and are ignored.

Stdlib + pyserial only.
    python hub.py --selftest
    python hub.py                    # auto: scan USB serial, listen for the Zynq beacon, HTTP :8090
"""
import argparse
import json
import math
import os
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import can_bridge  # noqa: E402  (same directory)

STALE_S = 3.0          # older than this and a value is flagged stale
HTTP_PORT = 8090
BEACON_PORT = 8092
ZYNQ_STATIC = ["10.20.0.2:8091"]          # the Zynq's fixed car-LAN address (its networkd config)
SCAN_S = 3.0

# USB vendor IDs worth asking "ID?": Teensy, Espressif native USB, WCH CH34x, Silicon Labs CP210x, FTDI
KNOWN_VIDS = {0x16C0, 0x303A, 0x1A86, 0x10C4, 0x0403}
ROLES = ("car-can-logger", "vent-display", "climate-node")
KEY_RE = re.compile(r"^[A-Z][A-Z0-9_]{0,31}$")


# ============================================================
#  store
# ============================================================
class Store:
    """Thread-safe key/value with per-key timestamps."""

    def __init__(self):
        self._lock = threading.Lock()
        self._v = {}          # key -> (value, monotonic_ts)
        self._faults = {}     # source -> message
        self._devices = {}    # role -> port

    def set(self, key, value):
        with self._lock:
            self._v[key] = (value, time.monotonic())

    def get(self, key, default=None):
        with self._lock:
            hit = self._v.get(key)
        return hit[0] if hit else default

    def age(self, key):
        with self._lock:
            hit = self._v.get(key)
        return (time.monotonic() - hit[1]) if hit else float("inf")

    def fault(self, source, msg):
        with self._lock:
            if msg is None:
                self._faults.pop(source, None)
            else:
                self._faults[source] = msg

    def device(self, role, port):
        with self._lock:
            if port is None:
                self._devices.pop(role, None)
            else:
                self._devices[role] = port

    def devices(self):
        with self._lock:
            return dict(self._devices)

    def snapshot(self):
        now = time.monotonic()
        with self._lock:
            out = {k: {"value": v, "age": round(now - t, 2),
                       "stale": (now - t) > STALE_S}
                   for k, (v, t) in self._v.items()}
            faults = dict(self._faults)
            devices = dict(self._devices)
        return {"values": out, "faults": faults, "devices": devices,
                "healthy": len(faults) == 0}


# ============================================================
#  line protocol
# ============================================================
def parse_kv_line(line):
    """'KEY=value' -> (KEY, float|str) or None. '#' lines, prose and junk are rejected, so a node's
    human-readable chatter can never create a key."""
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
        return None
    k, _, v = line.partition("=")
    k = k.strip().upper()
    v = v.strip()
    if not KEY_RE.match(k) or " " in v or not v:
        return None
    try:
        f = float(v)
        if math.isnan(f):
            return None              # "nan" = the node has no trustworthy reading: send nothing
        return k, f
    except ValueError:
        return k, v


# ============================================================
#  vent display formatting
# ============================================================
# The display parses "KEY=value\n". Only send keys it understands, and never
# send a stale value as if it were live -- send nothing and let it flag LINK.
DISPLAY_KEYS = ("SOC", "RANGE", "SPEED", "AIRT", "CASET", "VOLT",
                "DAMPER", "STAT")


def display_lines(store):
    out = []
    for k in DISPLAY_KEYS:
        if store.age(k) > STALE_S:
            continue                       # withhold rather than mislead
        v = store.get(k)
        if v is None:
            continue
        out.append(f"{k}={v}" if isinstance(v, str) else f"{k}={v:.1f}"
                   if isinstance(v, float) else f"{k}={v}")
    return out


def status_word(store):
    """What the display's STAT field shows: OK, or the first thing that is wrong (15 chars max)."""
    snap = store.snapshot()
    if snap["healthy"]:
        return "OK"
    return ("FLT " + sorted(snap["faults"])[0])[:15]


# ============================================================
#  serial plumbing
# ============================================================
def open_port(dev, baud=115200, timeout=1.0):
    """A real device path or a pyserial URL (socket://host:port in the tests)."""
    import serial
    if "://" in dev:
        return serial.serial_for_url(dev, baudrate=baud, timeout=timeout)
    return serial.Serial(dev, baud, timeout=timeout)


def candidate_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    return [p.device for p in list_ports.comports() if p.vid in KNOWN_VIDS]


def probe_role(dev, wait_s=2.0):
    """Ask a port who it is. Returns one of ROLES or None. Never raises."""
    try:
        with open_port(dev, timeout=0.2) as s:
            try:
                s.reset_input_buffer()
            except Exception:              # noqa: BLE001 - some URL handlers lack it
                pass
            s.write(b"\nID?\n")
            t_end = time.monotonic() + wait_s
            while time.monotonic() < t_end:
                line = s.readline().decode("ascii", "replace").strip()
                if line.startswith("ID="):
                    role = line[3:].strip()
                    return role if role in ROLES else None
    except Exception:                      # noqa: BLE001 - busy / vanished / not ours
        return None
    return None


class _PortWorker(threading.Thread):
    """Base for a worker that owns one serial port until the port dies."""

    daemon = True

    def __init__(self, store, dev, name, on_exit=None, baud=115200):
        super().__init__(name=f"{name}@{dev}")
        self.store, self.dev, self.name_, self.baud = store, dev, name, baud
        self.on_exit = on_exit
        self.stop_flag = False

    def run(self):
        try:
            with open_port(self.dev, self.baud) as s:
                self.store.fault(self.name_, None)
                self.store.device(self.name_, self.dev)
                self.work(s)
        except Exception as e:             # noqa: BLE001 - a dead source must not kill the hub
            self.store.fault(self.name_, f"{self.dev}: {e}")
        finally:
            self.store.device(self.name_, None)
            if not self.stop_flag:
                self.store.fault(self.name_, f"{self.dev}: lost")
            if self.on_exit:
                self.on_exit(self.dev)

    def lines(self, s):
        buf = b""
        while not self.stop_flag:
            chunk = s.read(s.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                yield raw.decode("ascii", "replace").strip()


class SerialSource(_PortWorker):
    """KEY=value lines from a node (the climate node). Values go straight into the store."""

    def __init__(self, store, dev, name="climate-node", prefix="", on_exit=None, baud=115200):
        super().__init__(store, dev, name, on_exit, baud)
        self.prefix = prefix

    def work(self, s):
        for line in self.lines(s):
            kv = parse_kv_line(line)
            if kv:
                self.store.set(self.prefix + kv[0], kv[1])


class CanSource(_PortWorker):
    """The Teensy CAN logger's raw stream -> named signals (signals.json) + link statistics.

    Sends 'R' (stream ON, idempotent) on connect -- never the 'r' toggle, which would switch a
    stream that is already running OFF. Publishes CAN_FPS (frames/s over the last second),
    CAN_IDS (distinct bus/id pairs seen) and every signal the map defines.
    """

    def __init__(self, store, dev, signals, name="car-can-logger", on_exit=None, baud=115200):
        super().__init__(store, dev, name, on_exit, baud)
        self.decoder = can_bridge.Decoder(signals)

    def work(self, s):
        s.write(b"R")
        ids, n, t0 = set(), 0, time.monotonic()
        self.store.set("CAN_FPS", 0.0)
        for line in self.lines(s):
            rec = can_bridge.parse_stream_line(line)
            now = time.monotonic()
            if rec:
                _, bus, cid, data = rec
                n += 1
                ids.add((bus, cid))
                for k, v in self.decoder.decode(bus, cid, data).items():
                    self.store.set(k, v)
            elif line.startswith("raw stream OFF"):
                s.write(b"R")                          # someone toggled it off: we need it on
            if now - t0 >= 1.0:
                self.store.set("CAN_FPS", round(n / (now - t0), 1))
                self.store.set("CAN_IDS", len(ids))
                n, t0 = 0, now


class DisplayWriter(_PortWorker):
    """Pushes the display keys (fresh ones only) to the vent Teensy at a fixed rate."""

    def __init__(self, store, dev, name="vent-display", hz=4, on_exit=None, baud=115200):
        super().__init__(store, dev, name, on_exit, baud)
        self.hz = hz

    def work(self, s):
        while not self.stop_flag:
            self.store.set("STAT", status_word(self.store))
            for ln in display_lines(self.store):
                s.write((ln + "\n").encode())
            s.flush()
            time.sleep(1.0 / self.hz)


class Discovery(threading.Thread):
    """Scans the USB serial ports, asks each new one 'ID?', starts the matching worker."""

    daemon = True

    def __init__(self, store, signals, extra_devs=(), scan_s=SCAN_S, use_comports=True):
        super().__init__(name="discovery")
        self.store, self.signals = store, signals
        self.extra = list(extra_devs)
        self.scan_s, self.use_comports = scan_s, use_comports
        self.owned = {}            # dev -> worker
        self.cooldown = {}         # dev -> monotonic time before which we do not re-probe
        self.lock = threading.Lock()
        self.stop_flag = False

    def _release(self, dev):
        with self.lock:
            self.owned.pop(dev, None)
            self.cooldown[dev] = time.monotonic() + 1.0

    def _start(self, role, dev):
        if role == "car-can-logger":
            w = CanSource(self.store, dev, self.signals, on_exit=self._release)
        elif role == "vent-display":
            w = DisplayWriter(self.store, dev, on_exit=self._release)
        else:
            w = SerialSource(self.store, dev, name=role, on_exit=self._release)
        with self.lock:
            self.owned[dev] = w
        w.start()

    def scan_once(self):
        devs = (candidate_ports() if self.use_comports else []) + self.extra
        roles_live = {w.name_ for w in self.owned.values()}
        for dev in devs:
            with self.lock:
                if dev in self.owned or time.monotonic() < self.cooldown.get(dev, 0):
                    continue
            role = probe_role(dev)
            if role is None or role in roles_live:
                self.cooldown[dev] = time.monotonic() + 10.0   # not ours / duplicate: look later
                continue
            self._start(role, dev)
            roles_live.add(role)
        for role in ROLES:
            if role not in roles_live and role not in self.store.snapshot()["faults"]:
                self.store.fault(role, "not found")

    def run(self):
        while not self.stop_flag:
            self.scan_once()
            time.sleep(self.scan_s)


# ============================================================
#  Zynq: beacon + TCP line source
# ============================================================
def parse_beacon(data):
    """b'ZYNQ-AGENT 8091 zynq' -> (port, host) or None."""
    try:
        parts = data.decode("ascii").split()
    except UnicodeDecodeError:
        return None
    if len(parts) >= 2 and parts[0] == "ZYNQ-AGENT" and parts[1].isdigit():
        return int(parts[1]), (parts[2] if len(parts) > 2 else "")
    return None


class ZynqFinder(threading.Thread):
    """Keeps exactly one connection to a zynq_agent: whoever beacons, else the static address.

    While connected it ingests KEY=value lines (ZYNQ_TEMP, PL_TIME, FAN_RPM, ...). A dropped link
    raises the 'zynq' fault and the search starts again -- nothing is left frozen in the store.
    """

    daemon = True

    def __init__(self, store, beacon_port=BEACON_PORT, static=tuple(ZYNQ_STATIC)):
        super().__init__(name="zynq")
        self.store, self.beacon_port = store, beacon_port
        self.static = [self._hp(x) for x in static]
        self.announced = {}                       # (host, port) -> last seen
        self.lock = threading.Lock()
        self.stop_flag = False

    @staticmethod
    def _hp(x):
        h, _, p = x.partition(":")
        return h, int(p or 8091)

    def _listen(self):
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        u.bind(("", self.beacon_port))
        u.settimeout(1.0)
        while not self.stop_flag:
            try:
                data, (ip, _) = u.recvfrom(256)
            except socket.timeout:
                continue
            b = parse_beacon(data)
            if b:
                with self.lock:
                    self.announced[(ip, b[0])] = time.monotonic()

    def candidates(self):
        now = time.monotonic()
        with self.lock:
            fresh = [hp for hp, t in self.announced.items() if now - t < 10]
        return fresh + [hp for hp in self.static if hp not in fresh]

    def _session(self, host, port):
        with socket.create_connection((host, port), timeout=2) as s:
            s.settimeout(5)
            self.store.fault("zynq", None)
            self.store.device("zynq", f"{host}:{port}")
            buf = b""
            while not self.stop_flag:
                chunk = s.recv(4096)
                if not chunk:
                    raise ConnectionError("closed")
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    kv = parse_kv_line(line.decode("ascii", "replace"))
                    if kv:
                        self.store.set(kv[0], kv[1])

    def run(self):
        threading.Thread(target=self._listen, name="zynq-beacon", daemon=True).start()
        self.store.fault("zynq", "searching")
        while not self.stop_flag:
            for host, port in self.candidates():
                try:
                    self._session(host, port)
                except Exception as e:         # noqa: BLE001
                    self.store.device("zynq", None)
                    self.store.fault("zynq", f"{host}:{port}: {e}")
                if self.stop_flag:
                    return
            time.sleep(1.0)


# kept for callers that point the hub at one fixed agent (tests, a Zynq on a known address)
class TcpSource(threading.Thread):
    """Reads 'KEY=value' lines from a TCP line server -- the Zynq's zynq_agent.py.

    Same contract as SerialSource: connect lazily, reconnect forever, never block the hub.
    Values arrive with the Zynq's own keys (ZYNQ_TEMP, PL_STATE, ...) and get the same
    age/staleness treatment as everything else, so a dead Zynq shows up as stale, not frozen.
    """

    daemon = True

    def __init__(self, store, host, port, name="zynq", prefix=""):
        super().__init__()
        self.store, self.host, self.port = store, host, int(port)
        self.name_, self.prefix = name, prefix
        self.stop_flag = False

    def run(self):
        while not self.stop_flag:
            try:
                with socket.create_connection((self.host, self.port), timeout=5) as s:
                    s.settimeout(5)
                    self.store.fault(self.name_, None)
                    buf = b""
                    while not self.stop_flag:
                        chunk = s.recv(4096)
                        if not chunk:
                            raise ConnectionError("closed")
                        buf += chunk
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            kv = parse_kv_line(line.decode("ascii", "replace"))
                            if kv:
                                self.store.set(self.prefix + kv[0], kv[1])
            except Exception as e:       # noqa: BLE001 - a dead source must not kill the hub
                self.store.fault(self.name_, str(e))
                time.sleep(2)


# ============================================================
#  HTTP api
# ============================================================
def make_handler(store):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype="application/json"):
            b = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_GET(self):
            if self.path.startswith("/api"):
                self._send(200, json.dumps(store.snapshot(), indent=1))
            elif self.path == "/health":
                snap = store.snapshot()
                self._send(200 if snap["healthy"] else 503,
                           json.dumps({"healthy": snap["healthy"],
                                       "faults": snap["faults"],
                                       "devices": snap["devices"]}))
            elif self.path == "/devices":
                self._send(200, json.dumps(store.devices()))
            else:
                self._send(404, "{}")
    return H


def start_hub(store, signals, http_port, extra_devs=(), use_comports=True, beacon_port=BEACON_PORT,
              zynq_static=tuple(ZYNQ_STATIC), scan_s=SCAN_S):
    """Everything the service runs. Returns (http_server, discovery, zynq_finder)."""
    disc = Discovery(store, signals, extra_devs, scan_s, use_comports)
    disc.start()
    zf = ZynqFinder(store, beacon_port, zynq_static)
    zf.start()
    srv = ThreadingHTTPServer(("0.0.0.0", http_port), make_handler(store))
    threading.Thread(target=srv.serve_forever, name="http", daemon=True).start()
    return srv, disc, zf


# ============================================================
#  selftest
# ============================================================
class _FakeNode(threading.Thread):
    """A TCP server that speaks one firmware's USB protocol, for pyserial socket:// URLs."""

    daemon = True

    def __init__(self, role, lines=(), period=0.05):
        super().__init__()
        self.role, self.lines, self.period = role, list(lines), period
        self.srv = socket.socket(); self.srv.bind(("127.0.0.1", 0)); self.srv.listen(4)
        self.url = f"socket://127.0.0.1:{self.srv.getsockname()[1]}"
        self.rx = b""
        self.stop_flag = False

    def run(self):
        while not self.stop_flag:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            c.settimeout(self.period)
            try:
                while not self.stop_flag:
                    try:
                        d = c.recv(4096)
                        if not d:
                            break
                        self.rx += d
                        if b"ID?" in d or (self.role == "car-can-logger" and b"I" in d):
                            c.sendall(f"ID={self.role}\r\n".encode())
                    except socket.timeout:
                        pass
                    for ln in self.lines:
                        c.sendall((ln + "\r\n").encode())
            except OSError:
                pass
            finally:
                c.close()

    def close(self):
        self.stop_flag = True
        self.srv.close()


def selftest():
    ok = True
    st = Store()

    print("1) staleness tracking")
    st.set("SOC", 64.5)
    if st.age("SOC") > 0.5:
        print("   FAIL: fresh value reported stale"); ok = False
    else:
        print("   ok: fresh value has near-zero age")
    if st.age("NOPE") != float("inf"):
        print("   FAIL: missing key should be infinitely old"); ok = False
    else:
        print("   ok: missing key -> infinite age")

    print("2) display output withholds stale values")
    st.set("VOLT", 14.1)
    st.set("DAMPER", "OPEN")
    lines = display_lines(st)
    if "SOC=64.5" not in lines or "DAMPER=OPEN" not in lines:
        print(f"   FAIL: expected keys missing from {lines}"); ok = False
    else:
        print(f"   ok: emits {lines}")
    st._v["SOC"] = (64.5, time.monotonic() - (STALE_S + 1))
    lines2 = display_lines(st)
    if any(ln.startswith("SOC=") for ln in lines2):
        print("   FAIL: stale SOC was still transmitted"); ok = False
    else:
        print("   ok: stale SOC withheld (display shows LINK instead of a lie)")

    print("3) faults are reported and clearable")
    st.fault("climate", "device missing")
    if st.snapshot()["healthy"] or status_word(st) != "FLT climate":
        print("   FAIL: healthy with an active fault"); ok = False
    else:
        print(f"   ok: unhealthy while a fault is set, STAT='{status_word(st)}'")
    st.fault("climate", None)
    if not st.snapshot()["healthy"] or status_word(st) != "OK":
        print("   FAIL: fault not cleared"); ok = False
    else:
        print("   ok: fault cleared -> healthy, STAT='OK'")

    print("4) line protocol: machine lines in, human lines out")
    cases = {"AIRT=18.4": ("AIRT", 18.4), "DAMPER=OPEN": ("DAMPER", "OPEN"), "caset=41": ("CASET", 41.0),
             "# FAULT air sensor -> damper CLOSED": None, "air=23.4C case=30.1C -> damper OPEN": None,
             "AIRT=nan": None, "garbage": None, "STATUS cap=ON sd=ok": None, "=5": None}
    bad = {ln: parse_kv_line(ln) for ln, want in cases.items() if parse_kv_line(ln) != want}
    if bad:
        print(f"   FAIL: {bad}"); ok = False
    else:
        print(f"   ok: {len(cases)} cases (prose, '#' lines, nan, junk rejected)")

    print("5) probe: each fake firmware is identified by its ID answer")
    nodes = {r: _FakeNode(r) for r in ROLES}
    for n in nodes.values():
        n.start()
    got = {r: probe_role(n.url, wait_s=1.5) for r, n in nodes.items()}
    if got != {r: r for r in ROLES}:
        print(f"   FAIL: {got}"); ok = False
    else:
        print(f"   ok: {got}")
    silent = socket.socket(); silent.bind(("127.0.0.1", 0)); silent.listen(1)
    if probe_role(f"socket://127.0.0.1:{silent.getsockname()[1]}", wait_s=0.5) is not None:
        print("   FAIL: a silent port was given a role"); ok = False
    else:
        print("   ok: a port that never answers gets no role")
    silent.close()
    for n in nodes.values():
        n.close()

    print("6) CAN stream -> named signal + link stats (and 'R', never the 'r' toggle)")
    frames = [f"R,{1000 + i},1,1A0,C8005A00" for i in range(5)] + ["STATUS cap=ON sd=ok"]
    fake = _FakeNode("car-can-logger", frames, period=0.02); fake.start()
    st2 = Store()
    w = CanSource(st2, fake.url, [{"key": "SOC", "bus": 1, "id": "1A0", "byte": 2, "width": 8,
                                   "scale": 0.5, "offset": 0}])
    w.start()
    t0 = time.monotonic()
    while (st2.get("SOC") is None or not st2.get("CAN_FPS")) and time.monotonic() - t0 < 4:
        time.sleep(0.05)
    w.stop_flag = True; fake.close()
    if st2.get("SOC") != 45.0 or not st2.get("CAN_FPS") or st2.get("CAN_IDS") != 1:
        print(f"   FAIL: SOC={st2.get('SOC')} FPS={st2.get('CAN_FPS')} IDS={st2.get('CAN_IDS')}"); ok = False
    elif b"R" not in fake.rx or b"r" in fake.rx:
        print(f"   FAIL: sent {fake.rx!r}"); ok = False
    else:
        print(f"   ok: SOC={st2.get('SOC')} CAN_FPS={st2.get('CAN_FPS')} CAN_IDS={st2.get('CAN_IDS')}, sent 'R'")

    print("7) Zynq beacon -> connection -> values; loss -> fault")
    agent = socket.socket(); agent.bind(("127.0.0.1", 0)); agent.listen(1)
    aport = agent.getsockname()[1]

    def serve_once():
        c, _ = agent.accept()
        c.sendall(b"ZYNQ_TEMP=46.9\nPL_STATE=operating\nnot a kv line\n")
        time.sleep(0.3)
        c.close()
    threading.Thread(target=serve_once, daemon=True).start()
    ub = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ub.bind(("127.0.0.1", 0))
    bport = ub.getsockname()[1]; ub.close()
    st3 = Store()
    zf = ZynqFinder(st3, beacon_port=bport, static=())
    zf.start()
    time.sleep(0.2)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    t0 = time.monotonic()
    while st3.get("ZYNQ_TEMP") is None and time.monotonic() - t0 < 4:
        tx.sendto(f"ZYNQ-AGENT {aport} zynq".encode(), ("127.0.0.1", bport))
        time.sleep(0.2)
    tx.close()
    if st3.get("ZYNQ_TEMP") != 46.9 or st3.get("PL_STATE") != "operating":
        print(f"   FAIL: {st3.get('ZYNQ_TEMP')} / {st3.get('PL_STATE')}"); ok = False
    else:
        print("   ok: beacon found the agent; ZYNQ_TEMP=46.9 PL_STATE=operating; junk line ignored")
    t0 = time.monotonic()
    while "zynq" not in st3.snapshot()["faults"] and time.monotonic() - t0 < 4:
        time.sleep(0.05)
    zf.stop_flag = True; agent.close()
    if "zynq" not in st3.snapshot()["faults"]:
        print("   FAIL: no fault after the agent went away"); ok = False
    else:
        print(f"   ok: fault raised: {st3.snapshot()['faults']['zynq']}")
    if parse_beacon(b"ZYNQ-AGENT 8091 zynq") != (8091, "zynq") or parse_beacon(b"HELLO") is not None:
        print("   FAIL: beacon parser"); ok = False

    print("8) snapshot shape")
    snap = st.snapshot()
    assert "values" in snap and "faults" in snap and "devices" in snap
    print(f"   ok: {len(snap['values'])} keys, json-serialisable ({len(json.dumps(snap))} bytes)")

    print("\nPASSED" if ok else "\nFAILED")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--dev", action="append", default=[],
                    help="extra serial device or pyserial URL to probe (repeatable)")
    ap.add_argument("--no-scan", action="store_true", help="do not scan USB serial ports")
    ap.add_argument("--zynq", action="append", default=None,
                    help=f"fallback host:port for zynq_agent (repeatable; default {ZYNQ_STATIC[0]})")
    ap.add_argument("--beacon-port", type=int, default=BEACON_PORT)
    ap.add_argument("--signals", default=can_bridge.DEFAULT_SIGNALS)
    ap.add_argument("--port", type=int, default=HTTP_PORT)
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)

    store = Store()
    signals = can_bridge.load_signals(a.signals)
    start_hub(store, signals, a.port, a.dev, not a.no_scan, a.beacon_port,
              tuple(a.zynq) if a.zynq else tuple(ZYNQ_STATIC))
    print(f"hub: http://0.0.0.0:{a.port}/api  ({len(signals)} CAN signal(s) defined)", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
