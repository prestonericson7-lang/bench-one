#!/usr/bin/env python3
"""
system_test.py -- the Pi software stack end to end, with no hardware attached.

Runs the REAL hub.py and the REAL zynq_agent.py as separate processes. The three USB nodes are
stood in for by TCP servers that speak each firmware's USB protocol exactly as the .ino files do
(pyserial reaches them with socket:// URLs, the same code path as /dev/ttyACM*):

  car-can-logger  one-letter commands: 'I' -> "ID=car-can-logger", 'R' stream on, 'X' off, 'r' TOGGLE;
                  "R,<t_us>,<bus>,<ID in upper hex>,<data hex>" frames, "STATUS ..." every 0.5 s
  vent-display    lines: "ID?" -> "ID=vent-display"; KEY=value lines are recorded (what the screen gets)
  climate-node    lines: "ID?" -> "ID=climate-node"; emits AIRT/CASET/DAMPER/CLIMATE_FAULT/CLIMATE_MODE
                  once a second plus '#' prose lines (which must never become keys)

Checks, in order:
  1. discovery: all three nodes identified by their ID answer, the Zynq found by its beacon
  2. data: a CAN frame decoded to SOC through signals.json; CAN_FPS > 0; climate keys; Zynq keys
  3. display: the vent display receives SOC, AIRT, CASET, DAMPER and STAT=OK -- and never "nan"
  4. unplug the climate node -> fault, STAT=FLT..., AIRT withheld once stale; replug -> healthy
  5. restart the hub while the logger is already streaming -> the stream stays ON (the 'r' toggle
     would have switched it OFF: that was a real bug)

    python system_test.py
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close()
    return p


class Node(threading.Thread):
    """TCP stand-in for one USB CDC device. Survives reconnects like the real port does."""

    daemon = True

    def __init__(self, role, port=0):
        super().__init__(name=role)
        self.role = role
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port)); self.srv.listen(4)
        self.port = self.srv.getsockname()[1]
        self.url = f"socket://127.0.0.1:{self.port}"
        self.stream = False          # logger state survives host reconnects (it is in the Teensy)
        self.rx_lines = []           # display: every line the hub sent
        self.linebuf = b""
        self.stop_flag = False
        self.t_us = 0

    # ---- per-role behaviour ----
    def on_bytes(self, c, data):
        if self.role == "car-can-logger":
            for ch in data.decode("ascii", "replace"):
                if ch == "I":
                    c.sendall(b"ID=car-can-logger\r\n")
                elif ch == "R":
                    self.stream = True; c.sendall(b"raw stream ON\r\n")
                elif ch == "X":
                    self.stream = False; c.sendall(b"raw stream OFF\r\n")
                elif ch == "r":
                    self.stream = not self.stream
                    c.sendall(f"raw stream {'ON' if self.stream else 'OFF'}\r\n".encode())
                elif ch == "?":
                    c.sendall(b"i=status d=census c=clear-census s=start/stop f=flush n=new-file "
                              b"m=remount-sd r=raw-stream(toggle) R=stream-on X=stream-off I=identity\r\n")
            return
        self.linebuf += data
        while b"\n" in self.linebuf:
            raw, self.linebuf = self.linebuf.split(b"\n", 1)
            line = raw.decode("ascii", "replace").strip()
            if line == "ID?":
                c.sendall(f"ID={self.role}\r\n".encode())
            elif self.role == "vent-display" and "=" in line:
                self.rx_lines.append(line)

    def tick(self, c):
        if self.role == "car-can-logger":
            if self.stream:
                for _ in range(10):                       # ~100 frames/s at a 0.1 s tick
                    self.t_us += 1000
                    c.sendall(f"R,{self.t_us},1,1A0,C8005A00\r\n".encode())
                    c.sendall(f"R,{self.t_us + 3},1,3F1,0102030405060708\r\n".encode())
            if self.t_us % 500000 < 20000:
                c.sendall(b"STATUS cap=ON sd=ok log=LOG00001.BIN bus1=1234 bus2=0 ids=2 dropped=0\r\n")
        elif self.role == "climate-node":
            c.sendall(b"AIRT=17.2\r\nCASET=38.5\r\nDAMPER=OPEN\r\nCLIMATE_FAULT=0\r\nCLIMATE_MODE=auto\r\n"
                      b"# air 17.2C case 38.5C -> damper OPEN\r\n# STATUS air 17.2C damper OPEN fault no\r\n")

    def run(self):
        while not self.stop_flag:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            c.settimeout(0.1)
            try:
                while not self.stop_flag:
                    try:
                        d = c.recv(4096)
                        if not d:
                            break
                        self.on_bytes(c, d)
                    except socket.timeout:
                        pass
                    self.tick(c)
            except OSError:
                pass
            finally:
                c.close()

    def unplug(self):
        self.stop_flag = True
        try:
            self.srv.close()
        except OSError:
            pass


def api(port):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/api", timeout=2) as r:
        return json.load(r)


def wait_for(pred, timeout, port):
    t0 = time.monotonic(); last = None
    while time.monotonic() - t0 < timeout:
        try:
            last = api(port)
            if pred(last):
                return last
        except Exception:                      # noqa: BLE001 - hub still starting
            pass
        time.sleep(0.25)
    return last


def val(snap, k):
    v = (snap or {}).get("values", {}).get(k)
    return v["value"] if v and not v["stale"] else None


def main():
    ok = True
    tmp = tempfile.mkdtemp()
    # signals.json for the test: SOC = byte 2 of 0x1A0 on bus 1, x0.5 -> 0x5A * 0.5 = 45.0
    sig = os.path.join(tmp, "signals.json")
    json.dump([{"key": "SOC", "bus": 1, "id": "1A0", "byte": 2, "width": 8, "scale": 0.5, "offset": 0}],
              open(sig, "w"))
    # fake sysfs for the agent
    iio = os.path.join(tmp, "sys", "bus", "iio", "devices", "iio_device0"); os.makedirs(iio)
    for n, v in (("in_temp0_raw", "2600"), ("in_temp0_offset", "-2219"), ("in_temp0_scale", "123.040771484")):
        open(os.path.join(iio, n), "w").write(v + "\n")
    os.makedirs(os.path.join(tmp, "proc"))
    open(os.path.join(tmp, "proc", "uptime"), "w").write("42.0 40.0\n")
    open(os.path.join(tmp, "proc", "meminfo"), "w").write("MemTotal: 1000000 kB\nMemAvailable: 900000 kB\n")

    nodes = {r: Node(r) for r in ("car-can-logger", "vent-display", "climate-node")}
    for n in nodes.values():
        n.start()
    http, bport, aport = free_port(), free_port(socket.SOCK_DGRAM), free_port()

    agent = subprocess.Popen([PY, os.path.join(HERE, "zynq_agent.py"), "--port", str(aport), "--bind", "127.0.0.1",
                              "--root", tmp, "--period", "0.2", "--beacon-to", f"127.0.0.1:{bport}"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    def start_hub():
        args = [PY, os.path.join(HERE, "hub.py"), "--no-scan", "--port", str(http), "--beacon-port", str(bport),
                "--zynq", "127.0.0.1:1", "--signals", sig]
        for n in nodes.values():
            args += ["--dev", n.url]
        return subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    hub = start_hub()
    try:
        print("1) discovery: three USB nodes by their ID answer, the Zynq by its beacon")
        snap = wait_for(lambda s: len(s["devices"]) == 4, 20, http)
        devs = (snap or {}).get("devices", {})
        if set(devs) != {"car-can-logger", "vent-display", "climate-node", "zynq"}:
            print(f"   FAIL: {devs}"); ok = False
        else:
            print(f"   ok: {devs}")

        print("2) data from every node")
        snap = wait_for(lambda s: val(s, "SOC") == 45.0 and (val(s, "CAN_FPS") or 0) > 0 and val(s, "AIRT") == 17.2
                        and val(s, "ZYNQ_TEMP") is not None and s["healthy"], 15, http)
        want = {"SOC": 45.0, "AIRT": 17.2, "CASET": 38.5, "DAMPER": "OPEN", "CLIMATE_FAULT": 0.0,
                "ZYNQ_TEMP": 46.9, "ZYNQ_UP": 42.0}
        got = {k: val(snap, k) for k in want}
        if got != want or not (val(snap, "CAN_FPS") or 0) > 0 or val(snap, "CAN_IDS") != 2 or not snap["healthy"]:
            print(f"   FAIL: {got} CAN_FPS={val(snap, 'CAN_FPS')} CAN_IDS={val(snap, 'CAN_IDS')} "
                  f"healthy={snap and snap['healthy']} faults={snap and snap['faults']}"); ok = False
        else:
            print(f"   ok: {got}, CAN_FPS={val(snap, 'CAN_FPS')}, CAN_IDS=2, healthy")
        prose = [k for k in snap["values"] if k.startswith(("#", "AIR ", "STATUS"))]
        if prose:
            print(f"   FAIL: prose became keys: {prose}"); ok = False

        print("3) what the vent display receives")
        time.sleep(1.0)
        rx = nodes["vent-display"].rx_lines
        keys = {ln.split("=")[0] for ln in rx}
        if not {"SOC", "AIRT", "CASET", "DAMPER", "STAT"} <= keys or "STAT=OK" not in rx:
            print(f"   FAIL: keys {sorted(keys)}; last {rx[-6:]}"); ok = False
        elif any(ln.endswith("=nan") for ln in rx):
            print("   FAIL: a nan reached the display"); ok = False
        else:
            print(f"   ok: {len(rx)} lines, keys {sorted(keys)}, STAT=OK")

        print("4) unplug the climate node, then plug it back in")
        cport = nodes["climate-node"].port
        nodes["climate-node"].unplug()
        snap = wait_for(lambda s: "climate-node" in s["faults"] and val(s, "AIRT") is None, 15, http)
        n0 = len(nodes["vent-display"].rx_lines)
        time.sleep(1.2)
        after = nodes["vent-display"].rx_lines[n0:]
        if not snap or "climate-node" not in snap["faults"] or val(snap, "AIRT") is not None:
            print(f"   FAIL: {snap and snap['faults']}"); ok = False
        elif any(ln.startswith("AIRT=") for ln in after) or not any(ln.startswith("STAT=FLT") for ln in after):
            print(f"   FAIL: display after unplug got {after[-6:]}"); ok = False
        else:
            print(f"   ok: fault '{snap['faults']['climate-node']}', AIRT withheld, display shows "
                  f"{[ln for ln in after if ln.startswith('STAT=')][-1]}")
        nodes["climate-node"] = Node("climate-node", cport); nodes["climate-node"].start()
        snap = wait_for(lambda s: s["healthy"] and val(s, "AIRT") == 17.2, 20, http)
        if not snap or not snap["healthy"]:
            print(f"   FAIL: not healthy after replug: {snap and snap['faults']}"); ok = False
        else:
            print("   ok: rediscovered, healthy again")

        print("5) restart the hub while the logger is already streaming")
        if not nodes["car-can-logger"].stream:
            print("   FAIL: stream was not on before the restart"); ok = False
        hub.terminate(); hub.wait(10)
        hub = start_hub()
        snap = wait_for(lambda s: (val(s, "CAN_FPS") or 0) > 0 and val(s, "SOC") == 45.0, 20, http)
        if not nodes["car-can-logger"].stream or not snap or not (val(snap, "CAN_FPS") or 0) > 0:
            print(f"   FAIL: stream={nodes['car-can-logger'].stream} CAN_FPS={val(snap, 'CAN_FPS')}"); ok = False
        else:
            print(f"   ok: stream still ON after the restart, CAN_FPS={val(snap, 'CAN_FPS')}")
    finally:
        hub.terminate(); agent.terminate()
        for n in nodes.values():
            n.unplug()

    print("\nSYSTEM TEST PASSED" if ok else "\nSYSTEM TEST FAILED")
    return ok


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
