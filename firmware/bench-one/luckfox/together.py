#!/usr/bin/env python3
"""
together.py -- the Luckfox driving the Teensy, which is the division of labour this machine is built on.

    the Teensy does the matrix arithmetic and nothing else
    the Luckfox holds the problem, issues the work, and collects the answers

The Teensy has 64 MB of 4-bit weights on a bus it drives itself, and a kernel measured at 2.07 cycles
per multiply-accumulate. This board has a Cortex-A7 with NEON, 33 MB of DDR2 at roughly 930 MB/s, and
an operating system. Neither is much use alone: the Teensy has the capacity and none of the
orchestration, this has the speed and a twentieth of the memory.

So this end owns the work queue and measures the result. It never touches the PSRAM bus and never
decides how the Teensy does its arithmetic, because a second actor on that bus is exactly what makes
a bench measurement meaningless.

    ./together.py [kilobytes-per-bank] [rounds]

Link: UART3 at /dev/ttyS3, 1 Mbaud, to the Teensy's Serial2 on pins 7 and 8.
"""

import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing. It ships with the stock Luckfox image; check the PATH.")

PORT = "/dev/ttyS3"
BAUD = 1000000


def ask(link, cmd, timeout=30.0):
    """One line out, one line back. No state is kept between requests at either end, so a reply that
    arrives late cannot be mistaken for the answer to the next question."""
    link.reset_input_buffer()
    link.write((cmd + "\n").encode())
    link.flush()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = link.read(256)
        if chunk:
            buf += chunk
            if b"\n" in buf:
                return buf.split(b"\n")[0].decode(errors="replace").strip()
    return None


def main():
    kb = int(sys.argv[1]) if len(sys.argv) > 1 else 1024
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 5

    link = serial.Serial(PORT, BAUD, timeout=0.5)
    time.sleep(0.2)

    info = ask(link, "I", timeout=60.0)
    if not info:
        sys.exit("no reply on %s. The Teensy answers only after its bring-up finishes, which takes\n"
                 "a few minutes on a cold start -- give it that, then check the two signal wires and\n"
                 "that both boards share a ground." % PORT)
    print("teensy: %s" % info)

    print("\n  round   partial        teensy ms    MB/s     MMAC/s    link ms")
    best = 0.0
    for r in range(rounds):
        t0 = time.time()
        reply = ask(link, "G %d" % kb, timeout=120.0)
        wall_ms = (time.time() - t0) * 1000.0
        if not reply or not reply.startswith("P"):
            print("  %5d   no reply (%r)" % (r, reply))
            continue

        parts = reply.split()
        partial = int(parts[1])
        us = int(parts[2])
        by = int(parts[3])

        ms = us / 1000.0
        mbps = by / (us / 1e6) / 1e6
        mmac = by * 2.0 / (us / 1e6) / 1e6
        best = max(best, mmac)

        print("  %5d   %-12d   %8.1f   %6.2f   %6.2f    %6.1f"
              % (r, partial, ms, mbps, mmac, wall_ms - ms))

    print("\n  best %.2f MMAC/s over %d MB of weights" % (best, (kb // 1024) * 8))
    print("  the gap between teensy ms and link ms is what orchestration costs;")
    print("  it is round-trip latency, not bandwidth, because no weights cross this wire.")
    link.close()


if __name__ == "__main__":
    main()
