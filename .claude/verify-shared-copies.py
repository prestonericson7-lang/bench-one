#!/usr/bin/env python3
"""Check that every sketch-local copy of a shared kernel matches shared/.

WHY THIS EXISTS
---------------
Arduino compiles every .c in a sketch folder and accepts no extra include path, so sketches that use
the project's real kernels keep local copies of them. `unpack_teensy/README.txt` says so plainly and
`build.bat` refreshes the copies from shared/ on every run, which is the right design.

It only works if the build script is the thing that builds. Calling `arduino-cli compile` directly --
which is what an agent reaches for, and what happened on 2026-09-11 -- skips the copy step and compiles
a stale kernel while reporting it as the shared one. That is the same fault as document 41's unrolled
loop and document 45's preprocessor condition: a number that describes code other than the code that
ships.

So this compares every local copy against its master, byte for byte modulo line endings, and says which
way each one is stale. Cheap, mechanical, and it does not depend on anyone remembering the README.

Run: python .claude/verify-shared-copies.py
Exits non-zero when any copy differs.
"""

import glob
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHARED = os.path.join(ROOT, 'firmware', 'bench-one', 'shared')


def norm(path):
    """Hash with line endings normalised: git's autocrlf rewrites these and it means nothing."""
    with open(path, 'rb') as f:
        data = f.read().replace(b'\r\n', b'\n')
    return hashlib.sha256(data).hexdigest(), len(data)


def main():
    if not os.path.isdir(SHARED):
        print('  shared/ not found at %s' % SHARED)
        return 1

    masters = {}
    for path in glob.glob(os.path.join(SHARED, '*.c')) + glob.glob(os.path.join(SHARED, '*.h')):
        masters[os.path.basename(path)] = path

    checked = 0
    stale = []
    for sketch in sorted(glob.glob(os.path.join(ROOT, 'firmware', 'bench-one', 'tests', '*'))):
        if not os.path.isdir(sketch):
            continue
        for name, master in masters.items():
            local = os.path.join(sketch, name)
            if not os.path.isfile(local):
                continue
            checked += 1
            lh, ln = norm(local)
            mh, mn = norm(master)
            rel = os.path.relpath(local, ROOT).replace('\\', '/')
            if lh == mh:
                print('  ok      %s' % rel)
            else:
                stale.append((rel, ln, mn))
                print('  STALE   %s  (%d bytes local, %d in shared)' % (rel, ln, mn))

    if checked == 0:
        print('  no sketch keeps a local copy of a shared kernel -- nothing to check')
        return 0

    if stale:
        print('\n  %d copy/copies differ from shared/. Refresh them before trusting any number' % len(stale))
        print('  measured from them:\n')
        for rel, _, _ in stale:
            d = os.path.dirname(rel)
            print('    cp firmware/bench-one/shared/%s %s/' % (os.path.basename(rel), d))
        print('\n  or run that sketch\'s build.bat, which does the copy itself. A sketch compiled')
        print('  without the copy step measures a stale kernel and reports it as the shared one.')
        return 1

    print('\n  every local copy matches shared/: each sketch measures the kernel it claims to')
    return 0


if __name__ == '__main__':
    sys.exit(main())
