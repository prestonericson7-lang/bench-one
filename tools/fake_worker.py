#!/usr/bin/env python3
"""
fake_worker.py -- a Teensy that does not exist, so the driver can be wrong on its own time.

WHY THIS EXISTS
---------------
Three benchmark windows in a row were lost to faults that had nothing to do with the hardware:

  * a patch to the link layer silently failed to apply, and the driver ran for another cycle with the
    old behaviour while the comments claimed otherwise
  * a reply left in the wire by a truncated run was read, rejected for not starting with the expected
    letter, and reported as "no reply on /dev/ttyS3. Check the two signal wires" -- while the board
    answered a direct probe perfectly on the same port seconds later
  * the run died with ENOSPC because the driver's own logging had filled a 67 MB root filesystem
  * the batch-32 pass was killed by the kernel and never produced the one number it was run for

Every one of those is a driver fault, and every one of them was found by spending a window of working
hardware on it. Hardware windows on this bench are scarce -- both boards have dropped off USB for whole
cycles at a time -- so the driver has no business discovering its own bugs there.

This speaks the worker's protocol well enough to exercise the parts of the driver that break: discovery,
qualification across all three bus modes, the full-span confirm, the stability gate, the benchmark at
several batch sizes, and the report. Run it with BENCH_PORT=stub.

WHAT IT DOES AND DOES NOT PROVE
-------------------------------
It proves the driver runs, makes sensible decisions, and does not crash. It proves nothing whatever about
the bus, the timing, the chips or the arithmetic -- the sums it returns are computed from the same rules
the host checks against, so the kernel check passes by construction and means nothing here. A number from
this file is not a measurement and must never be written down as one.

The bank behaviour it models is the board's measured behaviour, so the fallback paths get exercised:
CS0 and CS1 read in quad, five of the decoder banks only read one bit at a time, and one of them gives a
different answer when asked twice.
"""

import time


def _pat(i, tag):
    return (i * 0x9D + 0x3B + tag * 0x51) & 0xFF


def _activations():
    return [((i * 37) & 0x7F) - 64 for i in range(2048)]


# Which routes hold a chip, and the fastest bus mode each will read in: 0 quad, 2 single-bit 0x0B.
# This is the board as measured, including the bank that answers and then will not repeat itself.
_PRESENT = {
    (0, 0): 0, (1, 0): 0,
    (2, 0): 2, (2, 1): 2, (2, 2): 2, (2, 3): 2, (2, 4): 2, (2, 5): 2,
}
_FLAKY = (2, 4)

# The read index a bank needs in its own mode. Anything faster returns wrong bytes, which is what makes
# the qualification sweep do real work instead of accepting its first guess.
_NEEDS = {
    (0, 0): 13, (1, 0): 11,
    (2, 0): 4, (2, 1): 4, (2, 2): 5, (2, 3): 4, (2, 4): 4, (2, 5): 4,
}

_CYCLES_PER_BYTE = {0: 57.5, 1: 230.0, 2: 185.0}     # roughly what each mode measures
_FCPU = 600e6


class FakeSerial(object):
    """Enough of a pyserial Serial to drive the host with, and nothing more."""

    def __init__(self, port="stub", baud=1000000, timeout=0.5):
        self.timeout = timeout
        self._out = b""
        self._xv = _activations()
        self._xsum = sum(self._xv)
        self.mode = 0
        self.route = (0, 0)
        self.wi = 2
        self.ri = 4
        self.burst = 16
        self.tag = 0
        self.dummy = 6
        self.dummy1 = 8
        self.csu = 0
        self.gap = 0
        self.pad = 0
        self.quad_entered = False
        self._flaky_flip = 0

    # ---- the pyserial surface the driver uses -------------------------------------------------
    def reset_input_buffer(self):
        self._out = b""

    def flush(self):
        pass

    def close(self):
        pass

    def read(self, n=1):
        d, self._out = self._out[:n], self._out[n:]
        return d

    def read_until(self, terminator=b"\n", size=None):
        i = self._out.find(terminator)
        if i < 0:
            d, self._out = self._out, b""
            return d
        d, self._out = self._out[:i + 1], self._out[i + 1:]
        return d

    def write(self, data):
        for line in data.decode(errors="replace").replace("\r", "\n").split("\n"):
            if line.strip():
                self._handle(line.strip())
        return len(data)

    # ---- the worker's side -------------------------------------------------------------------
    def _say(self, s):
        self._out += (s + "\r\n").encode()

    def _arg(self, parts, i, default=0):
        try:
            return int(parts[i])
        except (IndexError, ValueError):
            return default

    def _readable(self):
        """Does the currently selected bank read correctly at the current settings."""
        want_mode = _PRESENT.get(self.route)
        if want_mode is None:
            return False
        if self.mode != want_mode:
            return False
        if self.mode != 0 and not self.quad_entered is False:
            pass
        if self.ri < _NEEDS[self.route]:
            return False
        return True

    def _sum(self, length, bits, batch):
        total = 0
        for j in range(length):
            b = _pat(j, self.tag)
            if bits == 4:
                total += ((b & 0x0F) - 8) * self._xv[(2 * j) % 2048]
                total += ((b >> 4) - 8) * self._xv[(2 * j + 1) % 2048]
            elif bits == 2:
                for f in range(4):
                    total += (((b >> (2 * f)) & 3) - 2) * self._xv[(4 * j + f) % 2048]
            else:
                for f in range(8):
                    total += (2 * ((b >> f) & 1) - 1) * self._xv[(8 * j + f) % 2048]
        total &= 0xFFFFFFFF
        if total >= 0x80000000:
            total -= 0x100000000
        return total

    def _cycles(self, length, bits, batch):
        per = _CYCLES_PER_BYTE[self.mode]
        overhead = 1.0 + (14.0 if self.mode == 0 else 40.0) / max(8, self.burst)
        kernel = 10.0 * batch if bits == 1 else (5.0 if bits == 2 else 2.5)
        return int(length * (per * overhead + kernel))

    def _handle(self, cmd):
        p = cmd.split()
        c = p[0][0].upper()

        if c == "I":
            self._say("I bench-one fake 1 nsel 10 banksz 8388608 blk 8192 bits 4,2,1 fcpu 600000000")
        elif c == "P":
            here = (self._arg(p, 1), self._arg(p, 2)) in _PRESENT
            self._say("P %d %d %d 83 50" % (1 if here else 0, 13 if here else 0, 93 if here else 0))
        elif c == "B":
            self.route = (self._arg(p, 1), self._arg(p, 2))
            self.quad_entered = (self.mode == 0)
            self._say("B ok")
        elif c == "E":
            self.route = (self._arg(p, 1), self._arg(p, 2))
            self._say("E ok")
        elif c == "Q":
            self.mode = self._arg(p, 1)
            self._say("Q %d" % self.mode)
        elif c == "T":
            self.wi, self.ri = self._arg(p, 1), self._arg(p, 2)
            self.burst = max(8, min(96, self._arg(p, 3, 16)))
            self._say("T %d %d %d" % (self.wi, self.ri, self.burst))
        elif c == "U":
            self.csu = self._arg(p, 1)
            self._say("U %d" % self.csu)
        elif c == "D":
            self.dummy = self._arg(p, 1)
            self._say("D %d" % self.dummy)
        elif c == "J":
            self.dummy1 = self._arg(p, 1)
            self._say("J %d" % self.dummy1)
        elif c == "K":
            self.pad = self._arg(p, 1)
            self._say("K %d 000100F9" % self.pad)
        elif c == "Y":
            self.gap = self._arg(p, 1)
            self._say("Y %d" % self.gap)
        elif c == "N":
            self.tag = self._arg(p, 1)
            self._say("N %d" % self.tag)
        elif c == "R" or c == "W":
            nb = max(4, min(8192, self._arg(p, 1, 32)))
            per = _CYCLES_PER_BYTE[self.mode]
            self._say("%s %d %d" % (c, nb, int(200 + nb * per)))
        elif c == "F":
            length = self._arg(p, 2)
            self._say("F %d" % self._cycles(length, 4, 1))
        elif c == "V":
            length = self._arg(p, 2)
            bad = 0 if self._readable() else length - 32
            self._say("V %d %d" % (bad, self._cycles(length, 4, 1)))
        elif c == "L":
            length = self._arg(p, 2)
            e = 0 if self._readable() else length // 2
            self._say("L %d %d %d %d %d" % (e, e, e, e, length * 2))
        elif c == "M":
            length, bits = self._arg(p, 2), self._arg(p, 3, 4)
            batch = self._arg(p, 4, 1)
            s = self._sum(length, bits, batch) * (1 if bits != 1 else 1)
            if not self._readable():
                s += 12345
            elif self.route == _FLAKY:
                # the bank that qualifies and then will not repeat itself
                self._flaky_flip += 1
                if self._flaky_flip % 4 == 0:
                    s += 97
            self._say("M %d %d" % (s, self._cycles(length, bits, batch)))
        elif c == "X":
            n = max(1, min(24, self._arg(p, 2, 8)))
            a = self._arg(p, 1)
            if self._readable():
                self._say("X " + " ".join("%02X" % _pat(a + i, self.tag) for i in range(n)))
            else:
                self._say("X " + " ".join("%02X" % 0x99 for _ in range(n)))
        elif c == "S" or c == "G":
            a = self._arg(p, 1)
            want = [_pat(a + i, self.tag) for i in range(4)]
            got = want if self.route in _PRESENT else [0, 0, 0, 0]
            self._say("%s %s want %s"
                      % (c, " ".join("%02X" % v for v in got),
                         " ".join("%02X" % v for v in want)))
        else:
            self._say("E unknown")


def open_stub(port, baud, timeout=0.5):
    return FakeSerial(port, baud, timeout)
