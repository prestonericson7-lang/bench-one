#!/usr/bin/env python3
"""
hub.py -- the telemetry hub. Runs on the Orange Pi; it is what makes the fleet
a system rather than a pile of boards.

    CAN logger (Teensy)  ┐
    climate node (ESP32) ├─>  hub  ─┬─>  vent display (Teensy)   KEY=value lines
    sentry cams (Luckfox)┘          ├─>  HTTP /api  (dashboard, phone)
    GPS                             └─>  voice assistant (current fix)

DESIGN RULES CARRIED FROM THE ARCHITECTURE
  * Every value carries an AGE. In a car, links drop constantly; a stale number
    presented as live is worse than no number. Consumers can see staleness and
    the vent display greys out / flags it.
  * Nothing here may block. A dead source must not stall the others, so all I/O
    is non-blocking with timeouts and every source is independently faulted.
  * Buffer locally, sync opportunistically. Never assume a consumer is up.

Stdlib only -- runs on the Pi, the Luckfox, or here.
    python hub.py --selftest
"""
import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STALE_S = 3.0          # older than this and a value is flagged stale
HTTP_PORT = 8090


# ============================================================
#  store
# ============================================================
class Store:
    """Thread-safe key/value with per-key timestamps."""

    def __init__(self):
        self._lock = threading.Lock()
        self._v = {}          # key -> (value, monotonic_ts)
        self._faults = {}     # source -> message

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

    def snapshot(self):
        now = time.monotonic()
        with self._lock:
            out = {k: {"value": v, "age": round(now - t, 2),
                       "stale": (now - t) > STALE_S}
                   for k, (v, t) in self._v.items()}
            faults = dict(self._faults)
        return {"values": out, "faults": faults,
                "healthy": len(faults) == 0}


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


# ============================================================
#  sources
# ============================================================
class SerialSource(threading.Thread):
    """Reads 'KEY=value' lines from a serial device (climate node, GPS, etc).

    Opened lazily and reopened on failure: a node that is unplugged and plugged
    back in must recover without restarting the hub.
    """

    daemon = True

    def __init__(self, store, dev, baud, name, prefix=""):
        super().__init__()
        self.store, self.dev, self.baud = store, dev, baud
        self.name_, self.prefix = name, prefix
        self.stop_flag = False

    def run(self):
        while not self.stop_flag:
            try:
                import serial            # optional; absent on a dev host
            except ImportError:
                self.store.fault(self.name_, "pyserial not installed")
                return
            try:
                with serial.Serial(self.dev, self.baud, timeout=1) as s:
                    self.store.fault(self.name_, None)
                    while not self.stop_flag:
                        line = s.readline().decode("ascii", "replace").strip()
                        if not line or "=" not in line:
                            continue
                        k, _, v = line.partition("=")
                        try:
                            v2 = float(v)
                        except ValueError:
                            v2 = v
                        self.store.set(self.prefix + k.strip().upper(), v2)
            except Exception as e:       # noqa: BLE001 - a dead source must not kill the hub
                self.store.fault(self.name_, str(e))
                time.sleep(2)


class DisplayWriter(threading.Thread):
    """Pushes the display keys out to the vent Teensy at a fixed rate."""

    daemon = True

    def __init__(self, store, dev, baud=115200, hz=4):
        super().__init__()
        self.store, self.dev, self.baud, self.hz = store, dev, baud, hz
        self.stop_flag = False

    def run(self):
        try:
            import serial
        except ImportError:
            self.store.fault("display", "pyserial not installed")
            return
        while not self.stop_flag:
            try:
                with serial.Serial(self.dev, self.baud, timeout=1) as s:
                    self.store.fault("display", None)
                    while not self.stop_flag:
                        for ln in display_lines(self.store):
                            s.write((ln + "\n").encode())
                        time.sleep(1.0 / self.hz)
            except Exception as e:       # noqa: BLE001
                self.store.fault("display", str(e))
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
                                       "faults": snap["faults"]}))
            else:
                self._send(404, "{}")
    return H


# ============================================================
#  selftest
# ============================================================
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
    # force staleness and confirm the value is withheld, not sent as live
    st._v["SOC"] = (64.5, time.monotonic() - (STALE_S + 1))
    lines2 = display_lines(st)
    if any(l.startswith("SOC=") for l in lines2):
        print("   FAIL: stale SOC was still transmitted"); ok = False
    else:
        print("   ok: stale SOC withheld (display shows LINK instead of a lie)")

    print("3) faults are reported and clearable")
    st.fault("climate", "device missing")
    if st.snapshot()["healthy"]:
        print("   FAIL: healthy with an active fault"); ok = False
    else:
        print("   ok: unhealthy while a fault is set")
    st.fault("climate", None)
    if not st.snapshot()["healthy"]:
        print("   FAIL: fault not cleared"); ok = False
    else:
        print("   ok: fault cleared -> healthy")

    print("4) snapshot shape")
    snap = st.snapshot()
    assert "values" in snap and "faults" in snap
    print(f"   ok: {len(snap['values'])} keys, json-serialisable "
          f"({len(json.dumps(snap))} bytes)")

    print("\nPASSED" if ok else "\nFAILED")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--climate", help="serial device of the ESP32 climate node")
    ap.add_argument("--display", help="serial device of the vent display Teensy")
    ap.add_argument("--port", type=int, default=HTTP_PORT)
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)

    store = Store()
    if a.climate:
        SerialSource(store, a.climate, 115200, "climate").start()
    if a.display:
        DisplayWriter(store, a.display).start()

    srv = ThreadingHTTPServer(("0.0.0.0", a.port), make_handler(store))
    print(f"hub: http://0.0.0.0:{a.port}/api")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
