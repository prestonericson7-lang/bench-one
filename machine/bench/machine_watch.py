#!/usr/bin/env python3
"""machine_watch.py -- the PC's whole job on bench day: listen to every console of the machine and
write everything down. It never runs the machine; the machine runs itself (machine-bench on FPGA #1).

Ports it watches (by USB identity, so the COM numbers do not matter):
  CH340   1A86:7523   the FPGA boards' consoles (J2). Labelled zynq? until the board says its hostname.
  Teensy  16C0:0483   psram_llm's serial.
  STM32   1A86:7523   also CH340 -- told apart by what they print (phase 2).
  ESP32-P4 303A:1001  USB-Serial-JTAG (phase 2).

What it types, and only this (the same as hardware/pz7020-starlite/linux/watch_boot.ps1):
  * "boot" if a U-Boot prompt 'Zynq> ' sits idle for 3 s (autoboot was interrupted)
  * nothing else. The boards log themselves in and print their own reports.

Output: bench-archive/<stamp>-machine/<label>.log (host-timestamped lines) and summary.md, rewritten
every 30 s and at exit (Ctrl-C).   python machine/bench/machine_watch.py
"""
import datetime
import os
import re
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pip install pyserial")

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STAMP = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
OUT = os.path.join(REPO, "bench-archive", f"{STAMP}-machine")
os.makedirs(OUT, exist_ok=True)
IDS = {(0x1A86, 0x7523): "ch340", (0x16C0, 0x0483): "teensy", (0x303A, 0x1001): "esp32p4"}
lock = threading.Lock()
state = {}          # label -> dict(port, lines, last, hostname, report)


def now():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def watch(dev, kind):
    label = f"{kind}-{dev}"
    st = state[label] = dict(port=dev, kind=kind, n=0, last="", host=None, report=[], inreport=False, uboot_at=None)
    path = os.path.join(OUT, f"{label}.log")
    try:
        s = serial.Serial(dev, 115200, timeout=0.5)
    except Exception as e:
        with open(path, "a") as f:
            f.write(f"{now()} | cannot open: {e}\n")
        return
    s.dtr = False
    s.rts = False
    buf = b""
    with open(path, "a", encoding="utf-8", errors="replace") as f:
        f.write(f"{now()} | watching {dev} as {kind}\n")
        while True:
            try:
                chunk = s.read(4096)
            except Exception as e:
                f.write(f"{now()} | read error: {e}\n"); f.flush(); time.sleep(1); continue
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    ln, buf = buf.split(b"\n", 1)
                    t = ln.decode("utf-8", "replace").rstrip("\r")
                    f.write(f"{now()} | {t}\n")
                    with lock:
                        st["n"] += 1; st["last"] = t
                        m = re.match(r"\s*(zynq\d)\s+login:|.*\s(zynq\d)\s*$|.*hostname[:= ]+(zynq\d)", t)
                        if m:
                            st["host"] = next(g for g in m.groups() if g)
                        if "ZYNQ-REPORT BEGIN" in t:
                            st["inreport"] = True; st["report"] = []
                        elif "ZYNQ-REPORT END" in t:
                            st["inreport"] = False
                        elif st["inreport"]:
                            st["report"].append(t)
                        if t.startswith("I bench-one psram_llm"):
                            st["host"] = "teensy"
                f.flush()
                if buf.endswith(b"Zynq> "):
                    st["uboot_at"] = st["uboot_at"] or time.time()
                else:
                    st["uboot_at"] = None
            elif st.get("uboot_at") and time.time() - st["uboot_at"] > 3:
                s.write(b"boot\r"); f.write(f"{now()} | >>> typed: boot\n"); f.flush(); st["uboot_at"] = None


def summary():
    rows = ["# machine watch " + STAMP, ""]
    for label, st in sorted(state.items()):
        rows.append(f"## {label}  ({st['host'] or 'unidentified'})  lines={st['n']}")
        rows.append(f"last: `{st['last'][:160]}`")
        if st["report"]:
            rows.append("```"); rows.extend(st["report"][:80]); rows.append("```")
        rows.append("")
    open(os.path.join(OUT, "summary.md"), "w", encoding="utf-8").write("\n".join(rows) + "\n")


def main():
    seen = set()
    print(f"watching; logs in {OUT}  (Ctrl-C to stop)")
    try:
        while True:
            for p in list_ports.comports():
                key = (p.vid, p.pid)
                if key in IDS and p.device not in seen:
                    seen.add(p.device)
                    threading.Thread(target=watch, args=(p.device, IDS[key]), daemon=True).start()
                    print(f"{now()} {p.device}: {IDS[key]}")
            time.sleep(2)
            if int(time.time()) % 30 == 0:
                with lock:
                    summary()
    except KeyboardInterrupt:
        pass
    with lock:
        summary()
    print(f"summary: {os.path.join(OUT, 'summary.md')}")


if __name__ == "__main__":
    main()
