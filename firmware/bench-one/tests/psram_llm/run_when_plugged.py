#!/usr/bin/env python3
"""run_when_plugged.py -- flash psram_llm once, and only onto the right board, when it appears.

    python firmware/bench-one/tests/psram_llm/run_when_plugged.py [hours] [log_seconds]

Waits until (1) the model card has left this PC's reader -- the Teensy halts without it -- and (2) a PJRC
board is on USB. Then asks the board what it is. Only a board answering "I bench-one" (the PSRAM worker
firmware) is flashed, through tools/bench_run.py, which archives the image and the whole serial log. A
silent board or anything else is NOT flashed: the owner has nine Teensys and a silent one is not proof of
the right one. Afterwards the log is checked against the PC reference with compare_run.py.

Exit: 0 run finished and compared, 1 compare found a divergence, 2 not flashed (wrong or silent board),
3 timed out waiting, 4 the harness failed."""
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))


def card_in_pc():
    for d in "DEFGHIJKLMNOPQRSTUVWXYZ":
        if os.path.exists("%s:\\qwen3b.gguf" % d):
            return "%s:" % d
    return None


def pjrc():
    from serial.tools import list_ports
    for p in list_ports.comports():
        if getattr(p, "vid", None) == 0x16C0:
            return p.device, getattr(p, "serial_number", None)
    return None, None


def ask(port):
    import serial
    try:
        s = serial.Serial(port, 115200, timeout=3)
        time.sleep(0.4)
        s.reset_input_buffer()
        s.write(b"I\n")
        s.flush()
        r = s.readline().decode(errors="replace").strip()
        s.close()
        return r
    except Exception as e:
        return "<could not ask: %s>" % e


def main():
    hours = float(sys.argv[1]) if len(sys.argv) > 1 else 12
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 7200
    deadline = time.time() + hours * 3600
    said = set()

    def once(key, msg):
        if key not in said:
            said.add(key)
            print(time.strftime("%H:%M:%S"), msg, flush=True)

    while time.time() < deadline:
        c = card_in_pc()
        port, sn = pjrc()
        if c:
            once("card", "waiting: the model card is still in this PC (%s); it goes in the Teensy's slot" % c)
        if not port:
            once("teensy", "waiting: no Teensy on USB yet")
        if c or not port:
            time.sleep(2)
            continue
        print(time.strftime("%H:%M:%S"), "Teensy on %s (serial %s), card out of the PC; asking it what it is" % (port, sn), flush=True)
        time.sleep(2)                     # let a board that just enumerated finish booting its firmware
        reply = ask(port)
        print(time.strftime("%H:%M:%S"), "it answered: %r" % reply, flush=True)
        if not reply.startswith("I bench-one"):
            print("NOT FLASHED: only a board answering 'I bench-one' (the PSRAM worker) is flashed from here.")
            print("If this is the PSRAM Teensy running other firmware, the owner confirms and it is flashed by hand.")
            return 2
        break
    else:
        print("timed out waiting")
        return 3

    note = "psram_llm first run: Qwen2.5-Coder-3B from SD, working memory in PSRAM, France prompt"
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "bench_run.py"),
                        os.path.join(HERE), str(secs), note], cwd=ROOT)
    if r.returncode != 0:
        print("harness failed, exit %d" % r.returncode)
        return 4
    runs = sorted(glob.glob(os.path.join(ROOT, "bench-archive", "*-psram_llm")))
    if not runs:
        print("no archive entry found")
        return 4
    log = os.path.join(runs[-1], "serial.log")
    print("\n==== compare with the PC reference: %s" % log, flush=True)
    return subprocess.run([sys.executable, os.path.join(HERE, "compare_run.py"), log]).returncode


if __name__ == "__main__":
    sys.exit(main())
