#!/usr/bin/env python3
"""boot_capture.py -- wait for the Teensy to come up (power-cycled by hand), log its whole boot, and hold it at READY.

    python boot_capture.py <archive dir> [--limit S]

The board prints its boot -- chips, card, model open, tokenizer store, attention-cache layout -- and then runs the
default prompt unless a line arrives within 30 s. This finds the PJRC USB port, re-finds it after every disconnect
(an unplug, a replug on a new COM number), logs every line host-timestamped to <archive dir>/serial.log, sends ::hold
as soon as READY appears, and exits 0 once the board answers HOLD; 1 if the board halts (the line says why) or nothing
comes within --limit."""
import os
import re
import sys
import time

import serial
from serial.tools import list_ports


def find():
    return next((p.device for p in list_ports.comports() if getattr(p, "vid", None) == 0x16C0), None)


def main():
    out = sys.argv[1]
    limit = float(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 3600.0
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "serial.log"), "a", encoding="utf-8")

    def note(s):
        log.write("%s | %s\n" % (time.strftime("%H:%M:%S"), s)); log.flush()
        print(s[:200], flush=True)

    t0 = time.time()
    s, held = None, False
    while time.time() - t0 < limit:
        if s is None:
            port = find()
            if not port:
                time.sleep(0.2)
                continue
            try:
                s = serial.Serial(port, 115200, timeout=0.5)
                note("[boot_capture] %s open" % port)
            except serial.SerialException:
                time.sleep(0.2)
                continue
        try:
            ln = s.readline().decode("utf-8", "replace").rstrip("\r\n")
        except (serial.SerialException, OSError):
            note("[boot_capture] disconnected; waiting for the board to come back")
            try:
                s.close()
            except Exception:
                pass
            s, held = None, False
            continue
        if not ln:
            continue
        note(ln)
        if "READY" in ln and not held:
            s.write(b"::hold\n")
            log.write("%s > ::hold\n" % time.strftime("%H:%M:%S")); log.flush()
            held = True
        if held and ln.startswith("HOLD"):
            return 0
        if re.search(r"HALTED", ln):
            return 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
