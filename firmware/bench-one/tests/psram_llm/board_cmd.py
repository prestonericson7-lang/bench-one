#!/usr/bin/env python3
"""board_cmd.py -- send board commands to an idle psram_llm and log every line it prints.

    python board_cmd.py <archive dir> "<command>" [<command> ...] [--until REGEX] [--wait S]

Waits for the board to be idle (answers "I"), holds the boot window if it is in one, then sends each
command in turn and keeps reading until --until matches (default: the next command's reply has ended, i.e.
two quiet seconds). Everything goes to <archive dir>/serial.log with a host timestamp, as suite.py does.
For one-off measurements such as ::sdsweep that are not a prompt test."""
import os
import re
import sys
import time

import serial
from serial.tools import list_ports


def main():
    args = sys.argv[1:]
    until, wait = None, 600.0
    if "--until" in args:
        i = args.index("--until"); until = args[i + 1]; del args[i:i + 2]
    if "--wait" in args:
        i = args.index("--wait"); wait = float(args[i + 1]); del args[i:i + 2]
    out, cmds = args[0], args[1:]
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "serial.log"), "a", encoding="utf-8")
    port = next(p.device for p in list_ports.comports() if getattr(p, "vid", None) == 0x16C0)
    s = serial.Serial(port, 115200, timeout=0.5)

    def rd(t, stop=None):
        t0, last, got = time.time(), time.time(), []
        while time.time() - t0 < t:
            line = s.readline().decode("utf-8", "replace").rstrip("\r\n")
            if line:
                last = time.time()
                got.append(line)
                log.write("%s | %s\n" % (time.strftime("%H:%M:%S"), line)); log.flush()
                print(line, flush=True)
                if stop and re.search(stop, line):
                    return got
            elif stop is None and time.time() - last > 2.0:
                return got
        return got

    def send(c):
        log.write("\n[board_cmd %s] >>> %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), c)); log.flush()
        s.write((c + "\n").encode()); s.flush()

    send("I")
    got = rd(900, r"READY\.|^I bench-one|DONE\.")
    if any("READY" in g for g in got):
        send("::hold"); rd(30, r"^HOLD")
        send("I"); rd(30, r"^I bench-one")
    for c in cmds:
        send(c)
        rd(wait, until)
    s.close()


if __name__ == "__main__":
    main()
