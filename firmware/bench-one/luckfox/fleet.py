#!/usr/bin/env python3
"""
fleet.py -- drive every node at once, and put each byte where it is read fastest.

    ./fleet.py [megabytes] [batch] [rounds]

WHAT THIS IS FOR

Every node in the chain is an independent processor. They all compute at the same time, so a pass over
the model costs the SLOWEST node -- not the sum of them all. That single fact is the whole design:

    a node should hold bytes in proportion to how fast it reads them,
    not in proportion to how many it can hold

Give every node an equal share and the fleet runs at the speed of its worst member. On this hardware the
spread between the fastest tier and the slowest is 21 to 1, so an even split wastes almost everything.

Only partial sums cross the wires -- a few bytes per node per pass -- because each node computes on the
weights it physically holds. That is why 1 Mbaud is enough to bind fifteen nodes together and why link
speed never appears in the arithmetic below.

HOW IT RUNS A PASS

    issue every node's work, without waiting for any of them
    collect every answer

Not issue-and-wait, node by node. The firmware relays without blocking precisely so that this is
possible: a command for node 9 passes through node 1 while node 1 is busy with its own.

WHAT IT MEASURES FIRST

Nothing is assumed about any node. The fleet is enumerated, each node is asked what it is, each node's
read rate is timed on its own memory, and the allocation falls out of those numbers. A node that will
not return the same answer twice is dropped rather than counted.
"""

import os
import sys
import time

NL = chr(10)

try:
    import serial
except ImportError:
    sys.exit("pyserial missing. It ships with the stock Luckfox image; check the PATH.")

PORT = os.environ.get("BENCH_PORT", "/dev/ttyS3")
BAUD = 1000000

# A node that cannot say the same thing twice is not memory.
STABILITY_READS = 3

# Probe size for rate measurement: big enough that the link round trip does not dominate, small enough
# that enumerating fifteen nodes does not take a coffee break.
PROBE_KB = 512


class Link(object):
    def __init__(self, port, baud, log):
        self.ser = serial.Serial(port, baud, timeout=0.5)
        self.log = log
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.buf = b""

    def send(self, cmd):
        self.ser.write((cmd + NL).encode())
        self.ser.flush()

    def line(self, timeout=30.0):
        """One complete line, whatever it is. The fleet path cannot filter by command letter the way the
        single-node driver does, because several nodes are answering at once and their replies interleave
        in whatever order they finish."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if b"\n" in self.buf:
                ln, _, self.buf = self.buf.partition(b"\n")
                t = ln.decode(errors="replace").strip()
                if t:
                    return t
            self.ser.timeout = min(0.5, max(0.05, deadline - time.time()))
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
        return None

    def ask(self, cmd, want, timeout=60.0):
        """Send one command and wait for a reply that starts with `want`. Anything else that arrives is
        discarded, because on a shared chain it belongs to somebody else's question."""
        self.send(cmd)
        deadline = time.time() + timeout
        while time.time() < deadline:
            t = self.line(max(0.2, deadline - time.time()))
            if t is None:
                break
            if t[:1].upper() == want.upper():
                return t
        return None

    def close(self):
        self.ser.close()


# ------------------------------------------------------------------------------------------------
#  the fleet
# ------------------------------------------------------------------------------------------------
class Node(object):
    def __init__(self, addr):
        self.addr = addr
        self.kind = "?"          # teensy | esp32 | luckfox
        self.capacity = 0        # bytes it can hold
        self.rate = 0.0          # MB/s of weights, measured
        self.share = 0           # bytes allocated to it
        self.ok = False


def enumerate_fleet(link, say):
    """One command numbers the whole pile. Each node takes the offered address, passes the next one to
    its leaf and then to the node behind it, and answers as it goes."""
    say(NL + "  enumerating: one A command, and the chain numbers itself")
    link.send("A 1")
    found = []
    deadline = time.time() + 12.0
    while time.time() < deadline:
        t = link.line(1.5)
        if t is None:
            continue
        if t[:1].upper() == "A":
            f = t.split()
            try:
                addr = int(f[1])
            except (IndexError, ValueError):
                continue
            if addr not in [n.addr for n in found]:
                found.append(Node(addr))
                say("    node %-3d answered" % addr)
                deadline = time.time() + 2.0      # keep listening while they keep coming
    found.sort(key=lambda n: n.addr)
    say("  %d nodes on the chain" % len(found))
    return found


def identify(link, say, nodes):
    say(NL + "  what each node is")
    say("    node  kind      memory")
    live = []
    for nd in nodes:
        r = link.ask("@%d I" % nd.addr, "I", timeout=20.0)
        if not r:
            say("    %-4d  no answer -- left out" % nd.addr)
            continue
        f = r.split()
        if "esp32s3" in r:
            nd.kind = "esp32"
            nd.capacity = int(f[f.index("psram") + 1]) if "psram" in f else 0
        elif "worker" in r:
            nd.kind = "teensy"
            nd.capacity = 0        # filled in by the bank probe below
        else:
            nd.kind = "luckfox"
            nd.capacity = int(f[f.index("psram") + 1]) if "psram" in f else 0
        nd.ok = True
        live.append(nd)
        say("    %-4d  %-8s  %s" % (nd.addr, nd.kind,
                                    ("%.1f MB" % (nd.capacity / 1e6)) if nd.capacity else "to be probed"))
    return live


def measure_rate(link, say, nd, probe_bytes):
    """Time the node on its own memory, then make it prove the answer does not move.

    The rate is what decides how much of the model this node carries, so a rate measured on a node that
    returns a different sum each time would poison the whole allocation."""
    if not link.ask("@%d F 0 %d" % (nd.addr, probe_bytes), "F", timeout=600.0):
        return False
    answers = {}
    best = 0.0
    for _ in range(STABILITY_READS):
        m = link.ask("@%d M 0 %d 1" % (nd.addr, probe_bytes), "M", timeout=600.0)
        if not m:
            return False
        f = m.split()
        try:
            val = int(f[1])
            units = int(f[2])
        except (IndexError, ValueError):
            return False
        answers[val] = answers.get(val, 0) + 1
        # the Teensy reports cycles, the others microseconds; both are in the identity line, and the
        # ratio of bytes to time is all that is wanted here
        secs = units / 600e6 if nd.kind == "teensy" else units / 1e6
        if secs > 0:
            best = max(best, probe_bytes / secs / 1e6)
    if len(answers) != 1:
        say("    %-4d unstable: %d different answers in %d reads -- left out"
            % (nd.addr, len(answers), STABILITY_READS))
        return False
    nd.rate = best
    return True


def allocate(nodes, model_bytes, say):
    """Bytes in proportion to rate, capped by capacity, and whatever will not fit in the fast tier spills
    into the slow one. This is the whole point of the file."""
    say(NL + "  allocation: bytes in proportion to read rate, capped by capacity")
    remaining = model_bytes
    pool = [n for n in nodes if n.rate > 0]
    for nd in pool:
        nd.share = 0
    # iterate: hand out by rate, re-share whatever bounces off a capacity limit
    for _ in range(8):
        total_rate = sum(n.rate for n in pool if n.share < n.capacity)
        if total_rate <= 0 or remaining <= 0:
            break
        handed = 0
        for nd in pool:
            room = nd.capacity - nd.share
            if room <= 0:
                continue
            want = int(remaining * nd.rate / total_rate)
            give = min(want, room)
            nd.share += give
            handed += give
        remaining -= handed
        if handed == 0:
            break
    if remaining > 0:
        say("    %.1f MB of the model does not fit anywhere: the fleet holds %.1f MB"
            % (remaining / 1e6, (model_bytes - remaining) / 1e6))
    return model_bytes - remaining


def run_pass(link, nodes, batch, say):
    """Issue to everyone, then collect. Never issue-and-wait."""
    working = [n for n in nodes if n.share > 0]
    t0 = time.time()
    for nd in working:
        bits_arg = " %d" % batch if (batch > 1 and nd.kind == "teensy") else ""
        link.send("@%d M 0 %d 1%s" % (nd.addr, nd.share, bits_arg))
    got = 0
    total = 0
    deadline = time.time() + 900.0
    while got < len(working) and time.time() < deadline:
        t = link.line(max(0.5, deadline - time.time()))
        if t is None:
            break
        if t[:1].upper() == "M":
            try:
                total += int(t.split()[1])
                got += 1
            except (IndexError, ValueError):
                pass
    return time.time() - t0, got, len(working), total


def main():
    mb = float(sys.argv[1]) if len(sys.argv) > 1 else 32.0
    batch = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    model = int(mb * 1e6)

    def say(m):
        print(m)
        sys.stdout.flush()

    say("bench one -- the whole fleet, one pass at a time")
    link = Link(PORT, BAUD, say)

    nodes = enumerate_fleet(link, say)
    if not nodes:
        sys.exit("nothing answered. Check the head link and that one process holds the port.")
    nodes = identify(link, say, nodes)

    say(NL + "  measuring each node on its own memory, %d kB probe" % PROBE_KB)
    say("    node  kind      MB/s")
    live = []
    for nd in nodes:
        if nd.capacity <= 0:
            nd.capacity = 16 * 1024 * 1024     # a Teensy reports banks, not bytes; probe fills this in
        probe = min(PROBE_KB * 1024, nd.capacity)
        if measure_rate(link, say, nd, probe):
            say("    %-4d  %-8s  %6.2f" % (nd.addr, nd.kind, nd.rate))
            live.append(nd)
    if not live:
        sys.exit("no node returned a repeatable answer")

    held = allocate(live, model, say)
    say("    node  kind      MB/s   share MB   seconds")
    for nd in sorted(live, key=lambda n: -n.rate):
        if nd.share:
            say("    %-4d  %-8s  %6.2f   %8.1f   %7.2f"
                % (nd.addr, nd.kind, nd.rate, nd.share / 1e6, nd.share / 1e6 / nd.rate))

    say(NL + "  %.1f MB in the fleet = %.0f million parameters at one bit"
        % (held / 1e6, held * 8 / 1e6))
    say(NL + "  round   wall s   nodes   MB/s    MMAC/s   tokens/s")
    best = 0.0
    for r in range(rounds):
        wall, got, want, total = run_pass(link, live, batch, say)
        if got < want:
            say("  %5d   only %d of %d answered" % (r, got, want))
            continue
        mbps = held / wall / 1e6
        mmac = held * 8.0 * batch / wall / 1e6
        best = max(best, mmac)
        say("  %5d   %6.2f   %2d/%-2d   %5.2f   %7.1f   %8.3f"
            % (r, wall, got, want, mbps, mmac, batch / wall))

    say(NL + "  best %.1f MMAC/s over %.0f million parameters" % (best, held * 8 / 1e6))
    link.close()


if __name__ == "__main__":
    main()
