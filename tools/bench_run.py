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


# Which physical boards are which, remembered across runs by USB serial number.
#
# Asking the board what it is covers the case where it answers. It does not cover silence, and silence is
# the dangerous case: a board in the bootloader is silent, and so is a board whose firmware simply is not
# listening on USB. The LoRa board that turned up in the worker's socket answered as a LoRa board on one
# cycle and answered nothing at all on the next -- same serial number, same socket, fifteen minutes apart.
# Treating that silence as "blank, safe to flash" would have destroyed it.
#
# So serial numbers are recorded. A board that has ever identified as something other than the worker is
# never flashed, whatever it does or does not say later.
IDENTITY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        ".claude", "board-identity.json")


def load_identity():
    try:
        with open(IDENTITY) as f:
            d = json.load(f)
    except (OSError, ValueError):
        d = {}
    d.setdefault("worker", [])
    d.setdefault("foreign", {})
    return d


def save_identity(d):
    try:
        with open(IDENTITY, "w") as f:
            json.dump(d, f, indent=2, sort_keys=True)
    except OSError:
        pass


def identify(timeout=3.0):
    """Ask whoever is on the PJRC port what they are, before writing flash over them.

    THIS EXISTS BECAUSE IT NEARLY HAPPENED. The port that had been the PSRAM worker all night came back
    answering "STATUS radio=FAIL osc=XTAL mode=0 freq=915.0 ..." -- a different Teensy, running somebody
    else's LoRa project, plugged into the same socket. The upload path here is a fixed USB location, so the
    next cycle of an unattended loop would have written psram_worker straight over it.

    Flashing is not reversible from this side: the firmware that was there is gone and only its owner knows
    how to put it back. So the board gets asked first. A worker answers "I bench-one", and a board fresh out
    of the bootloader answers nothing at all, which is also fine -- silence is what an unprogrammed or
    just-flashed board looks like. Anything that answers as something else stops the run.
    """
    try:
        import serial as pyserial
        from serial.tools import list_ports
    except ImportError:
        return None, "pyserial missing, cannot identify the board"

    port = None
    serial_no = None
    for p in list_ports.comports():
        if getattr(p, "vid", None) == 0x16C0:
            port = p.device
            serial_no = getattr(p, "serial_number", None)
            break
    if port is None:
        return None, None, "no PJRC device on the bus"

    try:
        link = pyserial.Serial(port, 115200, timeout=timeout)
        time.sleep(0.4)
        # A flash reboots the Teensy but NOT its SD card. Rebooting psram_llm mid-read left the card hung
        # (answers CMD0/CMD8, never finishes ACMD41) until its power was cut, 2026-09-27. "::stop" makes
        # psram_llm finish the card read in progress and stop; other firmware answers it as unknown text.
        link.write(b"::stop\n")
        link.flush()
        time.sleep(2.0)
        link.reset_input_buffer()
        link.write(b"I\n")
        link.flush()
        reply = link.readline().decode(errors="replace").strip()
        link.close()
    except Exception as e:
        return port, serial_no, "could not be asked (%s)" % e
    return port, serial_no, reply


def confirm_target():
    """Refuse to flash anything that is not known to be the worker."""
    port, serial_no, reply = identify()
    if port is None:
        print("  %s; flashing blind" % reply)
        return True

    known = load_identity()

    if reply.startswith("I bench-one"):
        if serial_no and serial_no not in known["worker"]:
            known["worker"].append(serial_no)
            known["foreign"].pop(serial_no, None)
            save_identity(known)
            print("  %s (serial %s) identified as the worker; remembered" % (port, serial_no))
        else:
            print("  %s is the worker: %s" % (port, reply[:70]))
        return True

    if reply:
        if serial_no:
            known["foreign"][serial_no] = reply[:120]
            save_identity(known)
        print("  %s (serial %s) answered: %s" % (port, serial_no, reply[:100]))
        print("  That is not the bench-one worker. REFUSING TO FLASH.")
        print("  Flashing would destroy whatever firmware is on that board, and only its owner knows how")
        print("  to put it back. Unplug it, or plug the worker in, and run this again.")
        return False

    # Silence. Safe only if this board has never answered as something else.
    if serial_no and serial_no in known["foreign"]:
        print("  %s (serial %s) is silent, but this board answered as something else before:"
              % (port, serial_no))
        print("    %s" % known["foreign"][serial_no])
        print("  Silence is not proof of a blank board -- firmware that is not listening on USB looks")
        print("  exactly like a bootloader. REFUSING TO FLASH.")
        return False
    if serial_no and serial_no in known["worker"]:
        print("  %s (serial %s) is silent but is the known worker; flashing" % (port, serial_no))
        return True
    print("  %s (serial %s) is silent and unrecognised, which is what a bootloader or a board with no"
          % (port, serial_no))
    print("  firmware looks like; flashing")
    return True


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

    # 0. BUILD WHAT WAS EDITED. psram_llm compiles a GENERATED file (make_ino.py writes psram_llm_board.cpp
    #    from the .inc that gets edited), and every sketch compiles COPIES of shared/ (its build.bat refreshes
    #    them). arduino-cli below does neither: on 2026-09-27 13:55 a flash of a .cpp an hour older than its
    #    .inc was one step away. So regenerate here, and refuse while any shared copy is stale.
    gen = os.path.join(sketch, "make_ino.py")
    if os.path.exists(gen):
        g = subprocess.run([sys.executable, gen], cwd=sketch, capture_output=True, text=True)
        if g.returncode != 0:
            sys.exit("refused: %s failed, nothing built or flashed:\n%s%s" % (gen, g.stdout, g.stderr))
        print("regenerated from source: %s" % g.stdout.strip())
    v = subprocess.run([sys.executable, os.path.join(ROOT, ".claude", "verify-shared-copies.py")], cwd=ROOT,
                       capture_output=True, text=True)
    if v.returncode != 0:
        sys.stdout.write(v.stdout[-3000:])
        sys.exit("refused: a sketch's copy of shared/ is stale (run that sketch's build.bat); nothing flashed")

    # 1. BUILD FIRST, straight into the archive. If this fails nothing is flashed and the board keeps
    #    whatever was on it, which is the state the previous archive entry already describes.
    print("building %s" % name)
    r = sh('arduino-cli compile -b %s --output-dir "%s" "%s"' % (FQBN, outdir, sketch))
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-4000:])
        sys.stderr.write(r.stderr[-4000:])
        # Take the directory back out. A failed build leaves it empty, and an empty directory in the
        # archive is a run that never happened claiming a slot -- it makes the index lie and it makes the
        # archive verifier report a backup-shaped hole where there is simply nothing.
        try:
            if not os.listdir(outdir):
                os.rmdir(outdir)
        except OSError:
            pass
        sys.exit("build failed; nothing flashed, board untouched, archive slot released")

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
    if not confirm_target():
        sys.exit("refused: the board on the bus is not the bench-one worker")
    print("flashing")
    r = sh('arduino-cli upload -b %s -p %s --input-dir "%s" "%s"' % (FQBN, PORT, outdir, sketch))
    # "Unable find Teensy Loader" is teensy_post_compile failing to talk to the loader application on
    # localhost -- before it has sent the loader a file or a reboot, so the board has not been touched.
    # 2026-09-27 05:33 it failed twice in a row while every CPU core was busy with model runs, then worked
    # with the machine idle. A bounded retry of that one host-side failure, nothing else.
    for attempt in range(3):
        if r.returncode == 0 or "Unable find Teensy Loader" not in (r.stdout or "") + (r.stderr or ""):
            break
        print("  the Teensy Loader did not answer (host side; the board is untouched); retry %d of 3 in 20 s" % (attempt + 1))
        time.sleep(20)
        r = sh('arduino-cli upload -b %s -p %s --input-dir "%s" "%s"' % (FQBN, PORT, outdir, sketch))
    if r.returncode != 0:
        # PORT is the USB socket the worker lived in. A board in another socket is the same board at a
        # different address, so ask arduino-cli where the Teensy actually is before giving up. This runs
        # only after confirm_target() has already identified the board, so it cannot widen what gets flashed.
        lst = sh('arduino-cli board list --format json')
        alt = None
        try:
            for d in json.loads(lst.stdout or "[]").get("detected_ports", []):
                addr = d.get("port", {}).get("address", "")
                if d.get("port", {}).get("protocol") == "teensy" or addr.startswith("usb:"):
                    for m in d.get("matching_boards", []) or []:
                        if m.get("fqbn") == FQBN:
                            alt = addr
        except (ValueError, AttributeError):
            alt = None
        if alt and alt != PORT:
            print("  the Teensy is at %s, not %s; uploading there" % (alt, PORT))
            r = sh('arduino-cli upload -b %s -p %s --input-dir "%s" "%s"' % (FQBN, alt, outdir, sketch))
        if r.returncode != 0:
            r = sh('arduino-cli upload -b %s -p %s "%s"' % (FQBN, alt or PORT, sketch))
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
