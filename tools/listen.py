#!/usr/bin/env python3
"""listen.py -- log everything the Teensy says on USB to a file, forever, until killed.

    python tools/listen.py <outfile>

The Teensy echoes every line in and out of the Luckfox link to USB, so this file is the board's own
record of an exchange it did not orchestrate. It exists because the control path is a UART the flashing
harness never sees, and the hard rule on this bench is that no measurement is ever lost.

The board is found by USB vendor ID 0x16C0 (PJRC) and re-found after every disconnect, because a
Teensy drops off the bus when it is flashed and comes back a second later, sometimes on a different
COM number. The name of this file matters: tools/bench_run.py releases the serial port by killing
processes whose command line contains "listen.py".
"""

import sys
import time

import serial
from serial.tools import list_ports


def find():
    for p in list_ports.comports():
        if getattr(p, "vid", None) == 0x16C0:
            return p.device
    return None


def main():
    out = open(sys.argv[1] if len(sys.argv) > 1 else "serial.log", "ab", 0)
    link = None
    while True:
        try:
            if link is None:
                port = find()
                if port is None:
                    time.sleep(0.5)
                    continue
                link = serial.Serial(port, 115200, timeout=0.5)
            data = link.read(4096)
            if data:
                out.write(data)
        except Exception:
            try:
                if link:
                    link.close()
            except Exception:
                pass
            link = None
            time.sleep(0.5)


if __name__ == "__main__":
    main()
