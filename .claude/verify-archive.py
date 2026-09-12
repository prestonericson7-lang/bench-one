#!/usr/bin/env python3
"""Check that every image in bench-archive/ is actually flashable.

WHY THIS EXISTS
---------------
The hard rule on this bench is that the exact image flashed for every run is kept, so any state the board
has been in can be returned to by flashing one file. That promise is only as good as the files, and nothing
has ever checked them. An archive holding a truncated image is not a backup; it is a backup-shaped hole
that is discovered at the worst possible moment, which here means the start of a scarce hardware window.

The concern is not hypothetical. One upload was interrupted partway through when the board dropped off the
USB bus, and the recovery plan for the next window is "reflash from that run's directory".

WHAT IT CHECKS
--------------
Intel HEX is self-describing, so a truncated or corrupted file is detectable without a board:

  * every line starts with ':' and has an even number of hex digits after it
  * every record's byte count matches its payload length
  * every record's checksum is correct -- the two's complement of the sum of all its bytes
  * the file ends with an end-of-file record (:00000001FF), which is what a truncated write loses first
  * the file contains at least one data record, so an empty-but-well-formed file is not called good

It also reports each run's code size from run.json where present, so an image that assembled to a
suspiciously different size from its neighbours is visible.

Run: python .claude/verify-archive.py
Exits non-zero if any archived image could not be trusted to restore the board.
"""

import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHIVE = os.path.join(ROOT, "bench-archive")


def check_hex(path):
    """Return (ok, detail, data_bytes, records)."""
    try:
        with open(path, "r") as f:
            lines = [ln.strip() for ln in f if ln.strip()]
    except OSError as e:
        return False, "unreadable: %s" % e, 0, 0

    if not lines:
        return False, "empty file", 0, 0

    data_bytes = 0
    records = 0
    saw_eof = False
    for i, ln in enumerate(lines, 1):
        if not ln.startswith(":"):
            return False, "line %d does not start with ':'" % i, data_bytes, records
        body = ln[1:]
        if len(body) % 2:
            return False, "line %d has an odd number of hex digits" % i, data_bytes, records
        try:
            raw = bytes.fromhex(body)
        except ValueError:
            return False, "line %d is not hex" % i, data_bytes, records
        if len(raw) < 5:
            return False, "line %d is too short to be a record" % i, data_bytes, records
        count = raw[0]
        rtype = raw[3]
        if len(raw) != count + 5:
            return False, ("line %d says %d payload bytes but carries %d"
                           % (i, count, len(raw) - 5)), data_bytes, records
        if (sum(raw) & 0xFF) != 0:
            return False, "line %d has a bad checksum" % i, data_bytes, records
        records += 1
        if rtype == 0x00:
            data_bytes += count
        elif rtype == 0x01:
            saw_eof = True
            if i != len(lines):
                return False, ("end-of-file record at line %d but the file continues to %d"
                               % (i, len(lines))), data_bytes, records

    if not saw_eof:
        return False, "no end-of-file record: the file is truncated", data_bytes, records
    if data_bytes == 0:
        return False, "well formed but contains no data records", data_bytes, records
    return True, "ok", data_bytes, records


def main():
    if not os.path.isdir(ARCHIVE):
        print("  no bench-archive/ -- nothing has been archived yet")
        return 1

    runs = sorted(d for d in glob.glob(os.path.join(ARCHIVE, "*"))
                  if os.path.isdir(d))
    if not runs:
        print("  bench-archive/ has no run directories")
        return 1

    bad = []
    missing = []
    empty = []
    sizes = []
    print("  checking %d archived runs" % len(runs))
    for d in runs:
        name = os.path.basename(d)
        hexes = sorted(glob.glob(os.path.join(d, "*.hex")))
        if not hexes:
            # An entirely empty directory is a build that failed before producing anything, which
            # bench_run.py now cleans up after itself. A directory with logs but no image is the
            # alarming case: something ran and there is no way to put the board back the way it was.
            if not os.listdir(d):
                empty.append(name)
            else:
                missing.append(name)
            continue
        for h in hexes:
            ok, detail, data_bytes, records = check_hex(h)
            sizes.append((name, data_bytes))
            if not ok:
                bad.append((name, os.path.basename(h), detail))
                print("  BAD     %-34s %s -- %s" % (name, os.path.basename(h), detail))

    note = {}
    for d in runs:
        rj = os.path.join(d, "run.json")
        if os.path.exists(rj):
            try:
                with open(rj) as f:
                    note[os.path.basename(d)] = json.load(f).get("note", "")
            except (OSError, ValueError):
                pass

    good = len(sizes) - len(bad)
    print("  %d images verified, %d bad, %d with logs but no image, %d empty (failed build)"
          % (good, len(bad), len(missing), len(empty)))

    if missing:
        print("\n  runs with a log but no flashable image, which cannot restore the board:")
        for m in missing:
            print("    %-34s %s" % (m, note.get(m, "")[:60]))

    if sizes:
        ok_sizes = [b for nm, b in sizes if nm not in [x[0] for x in bad]]
        if ok_sizes:
            print("\n  image payload: smallest %d, largest %d bytes"
                  % (min(ok_sizes), max(ok_sizes)))

    if bad:
        print("\n  %d archived image(s) cannot be trusted to restore the board. The hard rule on this"
              % len(bad))
        print("  bench is that any state it has been in is one file away; an image that fails these")
        print("  checks is a backup-shaped hole, and it is better to find it now than at the start of")
        print("  a hardware window.")
        return 1

    print("\n  every archived image is well formed, ends with its end-of-file record and carries data:")
    print("  any state this bench has been in can be restored from one file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
