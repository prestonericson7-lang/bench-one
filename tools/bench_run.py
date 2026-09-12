#!/usr/bin/env python3
"""
bench_run.py -- build, archive, flash, log. In that order, every time.

    python tools/bench_run.py <sketch-dir> [seconds] [note]

THE RULE THIS ENFORCES
----------------------
Nothing is flashed to the board until the exact binary about to be flashed has been saved, and no run
ends without its serial output saved beside that binary. So any state the bench has ever been in can
be returned to by re-flashing one file, and no measurement is ever lost because the next experiment
overwrote it.

That is what makes risky changes cheap. A timing setting pushed past where it has been verified, a
kernel rewritten in assembly, a bus driven harder than any sweep has blessed -- all of those are worth
trying when the cost of being wrong is re-flashing a file that is already on disk. Without the archive
every experiment carries the weight of the last working state, and that is what makes people stop
trying things.

WHAT IT WILL NOT DO
-------------------
It will not touch anything that can damage hardware. No overvolting, no clock changes outside what the
core supports, no disabling of thermal limits. The archive makes SOFTWARE risk cheap; it does nothing
about physical risk, and physical risk is not on the table.

WHAT LANDS ON DISK, PER RUN
---------------------------
    bench-archive/<stamp>-<sketch>/firmware.hex     the exact image that was flashed
    bench-archive/<stamp>-<sketch>/serial.log       everything the board said
    bench-archive/<stamp>-<sketch>/run.json         git commit, sketch, duration, note
    bench-archive/INDEX.md                          one line per run, newest last

The stamp sorts chronologically, so the directory listing is the history.
"""

import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHIVE = os.path.join(ROOT, "bench-archive")
FQBN = "teensy:avr:teensy41"
PORT = "usb:0/140000/0/B"


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)


def git_head():
    r = sh("git -C \"%s\" rev-parse --short HEAD" % ROOT)
    return r.stdout.strip() or "unknown"


def git_dirty():
    r = sh("git -C \"%s\" status --porcelain" % ROOT)
    return bool(r.stdout.strip())


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sketch = os.path.abspath(sys.argv[1])
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    note = sys.argv[3] if len(sys.argv) > 3 else ""
    name = os.path.basename(sketch.rstrip("\\/"))

    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(ARCHIVE, "%s-%s" % (stamp, name))
    os.makedirs(outdir, exist_ok=True)

    # 1. BUILD FIRST, straight into the archive. If this fails nothing is flashed and the board keeps
    #    whatever was on it, which is the state the previous archive entry already describes.
    print("building %s" % name)
    r = sh('arduino-cli compile -b %s --output-dir "%s" "%s"' % (FQBN, outdir, sketch))
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-4000:])
        sys.stderr.write(r.stderr[-4000:])
        sys.exit("build failed; nothing flashed, board untouched")

    hexes = [f for f in os.listdir(outdir) if f.endswith(".hex")]
    if not hexes:
        sys.exit("no .hex produced; refusing to flash something I cannot archive")
    hexfile = os.path.join(outdir, hexes[0])
    print("archived %s (%d bytes)" % (hexes[0], os.path.getsize(hexfile)))

    # 2. record what this build IS before it runs, so a log can always be traced to a tree state
    meta = {
        "stamp": stamp,
        "sketch": name,
        "git": git_head(),
        "git_dirty": git_dirty(),
        "seconds": secs,
        "note": note,
        "hex": hexes[0],
        "hex_bytes": os.path.getsize(hexfile),
    }
    with open(os.path.join(outdir, "run.json"), "w") as f:
        json.dump(meta, f, indent=2)

    # 3. release the port, flash the archived image
    # Kill the LISTENER, not every python on the machine.
    #
    # This script is python. "taskkill /F /IM python.exe /T" killed it mid-run, which is why it
    # produced no output and no serial.log and exited 1 every time. Match on the command line so only
    # the process holding the serial port dies.
    sh('powershell -NoProfile -Command "Get-CimInstance Win32_Process '
       '| Where-Object { $_.CommandLine -like \'*listen.py*\' } '
       '| ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"')
    time.sleep(1.5)
    print("flashing")
    r = sh('arduino-cli upload -b %s -p %s --input-dir "%s" "%s"' % (FQBN, PORT, outdir, sketch))
    if r.returncode != 0:
        r = sh('arduino-cli upload -b %s -p %s "%s"' % (FQBN, PORT, sketch))
        if r.returncode != 0:
            sys.stderr.write(r.stderr[-2000:])
            sys.exit("upload failed; the archived hex is still on disk and can be flashed by hand")

    # 4. log everything the board says, for the whole window, to the archive
    print("logging %d s" % secs)
    logpath = os.path.join(outdir, "serial.log")
    try:
        import serial as pyserial
        from serial.tools import list_ports

        # Find the board by USB vendor ID, not by its description string.
        #
        # 0x16C0 is PJRC. The description is whatever Windows decided to call it that day and it is
        # shared with every other USB serial adapter on the machine, so matching on it finds the
        # wrong device or none. This is the method the working listener uses and there is no reason
        # to invent a second one.
        def find_port():
            for p in list_ports.comports():
                if getattr(p, "vid", None) == 0x16C0:
                    return p.device
            return None

        # Look for the board EVERY time, not once.
        #
        # Flashing takes the device off the bus and it re-enumerates a second or two later, sometimes
        # on a different COM number. A single lookup at the top of this function runs while the board
        # is still gone, finds nothing, and throws away the whole run's log -- which is exactly what
        # happened. The lookup belongs inside the retry loop with the open.
        deadline = time.time() + secs
        with open(logpath, "wb") as out:
            link = None
            while time.time() < deadline:
                try:
                    if link is None:
                        port = find_port()
                        if port is None:
                            time.sleep(0.5)
                            continue
                        link = pyserial.Serial(port, 115200, timeout=0.5)
                    data = link.read(4096)
                    if data:
                        out.write(data)
                        out.flush()
                except Exception:
                    try:
                        if link:
                            link.close()
                    except Exception:
                        pass
                    link = None
                    time.sleep(0.5)   # the board reboots on flash; reopen rather than give up
    except Exception as e:
        with open(logpath, "a") as out:
            out.write("\n[logger failed: %s]\n" % e)

    size = os.path.getsize(logpath) if os.path.exists(logpath) else 0
    print("logged %d bytes to %s" % (size, logpath))

    # 5. one line in the index, so the history reads without opening anything
    os.makedirs(ARCHIVE, exist_ok=True)
    idx = os.path.join(ARCHIVE, "INDEX.md")
    if not os.path.exists(idx):
        with open(idx, "w") as f:
            f.write("# Bench archive\n\n"
                    "Every run's exact firmware image and full serial log, so any state the bench has\n"
                    "been in can be returned to by flashing one file. Newest last.\n\n"
                    "| stamp | sketch | git | log bytes | note |\n|---|---|---|---|---|\n")
    with open(idx, "a") as f:
        f.write("| %s | %s | %s%s | %d | %s |\n"
                % (stamp, name, meta["git"], "+dirty" if meta["git_dirty"] else "", size, note))

    print("run.json, firmware.hex and serial.log are in %s" % outdir)


if __name__ == "__main__":
    main()
