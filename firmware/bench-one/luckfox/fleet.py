#!/usr/bin/env python3
"""
fleet.py -- drive every node at once, and put each byte where it is read fastest.

    ./fleet.py [megabytes] [batch] [rounds]

THE ONE IDEA

Every node is an independent processor, so they all compute at the same time and a pass costs the
SLOWEST node rather than the sum of them. Therefore:

    a node holds bytes in proportion to how fast it reads them,
    not in proportion to how many it can hold

The measured spread across the tiers on this hardware is 21 to 1, so an even split runs the whole fleet
at the speed of its worst member and wastes almost everything else.

Only partial sums cross the wires -- a few bytes per node per pass -- because each node computes on the
weights it physically holds. A megabyte at 1 Mbaud is eight seconds; the host sends the RULE that
generates the weights instead, and every node materialises them at its own memory speed. That is why
link speed never appears in any of the arithmetic here.

WHAT THIS FIXES OVER THE FIRST DRAFT, all four of which would have broken a live run:

  * The Teensy reports CYCLES, not microseconds, and it now runs at 816 MHz. The draft divided by a
    hardcoded 600e6 and would have reported every Teensy 36% faster than it is -- which then feeds the
    allocation, so the error compounds into putting too much of the model on the slowest tier.
  * A Teensy's F, V and M act on the SELECTED bank. The draft never sent B, so every Teensy would have
    been measured on whatever bank happened to be selected, or none.
  * A Teensy holds up to ten banks of 8 MB and the draft treated it as one flat 16 MB. Work has to be
    issued per bank, and how many banks a node has is discovered by probing, never assumed.
  * A pass must issue to every node before collecting from any. The draft did that; this keeps it, and
    adds the per-node job queue that a multi-bank node needs -- a node with four banks gets its next
    bank the moment its last one answers, while every other node is still working.

WHAT IT ASSUMES ABOUT NOTHING

The fleet is enumerated, every node is asked what it is, every bank is probed for a chip, every timing
is swept until the data reads back, and every rate is timed. A node that will not return the same answer
twice is dropped rather than counted.
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

NOPS = [4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24]
USABLE = 15
MODES = [(0, "quad"), (2, "single 0x0B"), (1, "single 0x03")]
SETUPS = [0, 16]
SAFE_BURST = 16
WORK_BURST = 72
WAITS_QUAD = 6
WAITS_0B = 8
BANKSZ = 8 * 1024 * 1024

QUAL_KB = 64          # sweep span: cheap, and only has to reject settings that are obviously wrong
CONFIRM_KB = 512      # then prove it over something long enough to mean something
CONFIRM_PASSES = 2
STABILITY_READS = 3


class Link(object):
    """One reader, one writer, and replies matched to questions.

    Several nodes answer at once on a shared chain, so their lines interleave in whatever order they
    finish. Nothing here may assume the next line belongs to the last command."""

    def __init__(self, port, baud):
        if "://" in port:
            self.ser = serial.serial_for_url(port, baudrate=baud, timeout=0.5)
        else:
            self.ser = serial.Serial(port, baud, timeout=0.5)
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.buf = b""

    def send(self, cmd):
        self.ser.write((cmd + NL).encode())
        self.ser.flush()

    def line(self, timeout=30.0):
        deadline = time.time() + timeout
        while True:
            if b"\n" in self.buf:
                ln, _, self.buf = self.buf.partition(b"\n")
                t = ln.decode(errors="replace").strip()
                if t:
                    return t
            left = deadline - time.time()
            if left <= 0:
                return None
            self.ser.timeout = min(0.5, max(0.05, left))
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk

    def ask(self, cmd, want, timeout=60.0):
        self.send(cmd)
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                return None
            t = self.line(left)
            if t is None:
                return None
            if t[:1].upper() == want.upper():
                return t

    def close(self):
        self.ser.close()


class Bank(object):
    """One addressable lump of memory. A Luckfox or an ESP32 has one; a Teensy has as many as it has
    chips, and each carries its own timing because they are not the same chip on the same wiring."""

    def __init__(self, node, kind, y, size):
        self.node = node
        self.kind = kind          # select route on a Teensy; 0 for a single-memory node
        self.y = y
        self.size = size
        self.mode = 0
        self.wi = 7
        self.ri = USABLE - 1
        self.nb = SAFE_BURST
        self.su = 0
        self.rate = 0.0
        self.share = 0

    @property
    def name(self):
        if self.node.kind != "teensy":
            return "%d" % self.node.addr
        return "%d.%s" % (self.node.addr, ["CS0", "CS1", "Y%d" % self.y][min(self.kind, 2)])


class Node(object):
    def __init__(self, addr):
        self.addr = addr
        self.kind = "?"
        self.fcpu = 600e6
        self.banks = []
        self.queue = []
        self.busy = False


def secs_from(node, units):
    """The Teensy counts cycles because a microsecond is too coarse for a 96-byte burst; everything else
    counts microseconds. Which one is decided by what the node said it was, not by a constant."""
    return units / node.fcpu if node.kind == "teensy" else units / 1e6


# ------------------------------------------------------------------------------------------------
def enumerate_fleet(link, say):
    say(NL + "  enumerating: one command, and the stack numbers itself")
    link.send("A 1")
    found = {}
    deadline = time.time() + 15.0
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
            if addr not in found:
                found[addr] = Node(addr)
                say("    node %-3d answered" % addr)
                deadline = time.time() + 2.5
    nodes = [found[a] for a in sorted(found)]
    say("  %d nodes" % len(nodes))
    return nodes


def identify(link, say, nodes):
    say(NL + "  what each node is")
    say("    node  kind      clock       memory")
    live = []
    for nd in nodes:
        r = link.ask("@%d I" % nd.addr, "I", timeout=20.0)
        if not r:
            say("    %-4d  no answer -- left out" % nd.addr)
            continue
        f = r.split()
        if "fcpu" in f:
            try:
                nd.fcpu = float(f[f.index("fcpu") + 1])
            except (IndexError, ValueError):
                pass
        psram = 0
        if "psram" in f:
            try:
                psram = int(f[f.index("psram") + 1])
            except (IndexError, ValueError):
                psram = 0
        if "esp32s3" in r:
            nd.kind = "esp32"
            nd.banks = [Bank(nd, 0, 0, psram)]
        elif "luckfox" in r:
            nd.kind = "luckfox"
            nd.banks = [Bank(nd, 0, 0, psram)]
        else:
            nd.kind = "teensy"      # banks discovered by probing, never assumed
        live.append(nd)
        say("    %-4d  %-8s  %6.0f MHz  %s"
            % (nd.addr, nd.kind, nd.fcpu / 1e6,
               "%.1f MB" % (psram / 1e6) if psram else "probe for chips"))
    return live


def probe_teensy_banks(link, say, nd):
    """Ten possible selects, and a real ESP-PSRAM64H answers 0x0D then 0x5D where an open bus cannot.

    This is how one firmware image serves a node with eight chips and a node with two: nobody is told,
    every node is asked."""
    routes = [(0, 0), (1, 0)] + [(2, y) for y in range(8)]
    for kind, y in routes:
        r = link.ask("@%d P %d %d" % (nd.addr, kind, y), "P", timeout=30.0)
        if r and r.split()[1] == "1":
            nd.banks.append(Bank(nd, kind, y, BANKSZ))
    say("    node %-3d  %d chips = %d MB" % (nd.addr, len(nd.banks), len(nd.banks) * 8))


def select(link, bk):
    """Put a bank in force. Every knob, every time -- a worker that keeps a setting the host did not
    send is a worker that drifts, and a diagnostic that left the quad wait count at 96 once cost a whole
    benchmark run before that rule was adopted."""
    nd = bk.node
    if nd.kind != "teensy":
        return
    link.ask("@%d Q %d" % (nd.addr, bk.mode), "Q", timeout=15.0)
    link.ask("@%d D %d" % (nd.addr, WAITS_QUAD), "D", timeout=15.0)
    if bk.mode == 2:
        link.ask("@%d J %d" % (nd.addr, WAITS_0B), "J", timeout=15.0)
    link.ask("@%d Y 0" % nd.addr, "Y", timeout=15.0)
    link.ask("@%d U %d" % (nd.addr, bk.su), "U", timeout=15.0)
    link.ask("@%d B %d %d" % (nd.addr, bk.kind, bk.y), "B", timeout=30.0)
    link.ask("@%d T %d %d %d" % (nd.addr, bk.wi, bk.ri, bk.nb), "T", timeout=15.0)


def clean(link, bk, nbytes, passes=1):
    a = bk.node.addr
    if not link.ask("@%d F 0 %d" % (a, nbytes), "F", timeout=900.0):
        return False
    for _ in range(passes):
        v = link.ask("@%d V 0 %d" % (a, nbytes), "V", timeout=900.0)
        if not v:
            return False
        try:
            if int(v.split()[1]) != 0:
                return False
        except (IndexError, ValueError):
            return False
    return True


def qualify_bank(link, say, bk):
    """Quad first because it is four bits to the clock; single-bit second because on six of eight chips
    here it is the only thing that reads. Then the setting has to survive a span long enough to mean
    something, because a sweep that is silent over 64 kB has only bounded the error rate, not proved it."""
    if bk.node.kind != "teensy":
        bk.rate = 0.0
        return True
    probe = QUAL_KB * 1024
    for mode, mlabel in MODES:
        for su in SETUPS:
            bk.mode, bk.su, bk.wi, bk.nb = mode, su, 7, SAFE_BURST
            found = None
            for ri in range(USABLE):
                bk.ri = ri
                select(link, bk)
                if clean(link, bk, probe):
                    found = ri
                    break
            if found is None:
                continue
            for wi in range(USABLE):
                bk.wi = wi
                select(link, bk)
                if clean(link, bk, probe):
                    break
            bk.nb = WORK_BURST
            for _ in range(6):
                select(link, bk)
                if clean(link, bk, CONFIRM_KB * 1024, CONFIRM_PASSES):
                    say("      %-8s %-11s write %2d read %2d burst %2d setup %2d"
                        % (bk.name, mlabel, NOPS[bk.wi], NOPS[bk.ri], bk.nb, bk.su))
                    return True
                if bk.ri + 1 < USABLE:
                    bk.ri += 1
                elif bk.wi + 1 < USABLE:
                    bk.wi += 1
                else:
                    break
    say("      %-8s nothing reads it back -- left out" % bk.name)
    return False


def measure(link, say, bk):
    """Time it, then make it say the same thing three times. The rate decides how much of the model this
    bank carries, so a rate taken from a bank whose answer moves would poison the whole allocation."""
    nd = bk.node
    n = min(CONFIRM_KB * 1024, bk.size)
    select(link, bk)
    if not link.ask("@%d F 0 %d" % (nd.addr, n), "F", timeout=900.0):
        return False
    answers, best = {}, 0.0
    for _ in range(STABILITY_READS):
        m = link.ask("@%d M 0 %d 1" % (nd.addr, n), "M", timeout=900.0)
        if not m:
            return False
        f = m.split()
        try:
            answers[int(f[1])] = 1
            s = secs_from(nd, int(f[2]))
        except (IndexError, ValueError):
            return False
        if s > 0:
            best = max(best, n / s / 1e6)
    if len(answers) != 1:
        say("      %-8s unstable: %d answers in %d reads -- left out"
            % (bk.name, len(answers), STABILITY_READS))
        return False
    bk.rate = best
    return True


def allocate(banks, model_bytes, say):
    say(NL + "  allocation: bytes in proportion to read rate, capped by capacity")
    for bk in banks:
        bk.share = 0
    remaining = model_bytes
    for _ in range(10):
        pool = [b for b in banks if b.share < b.size and b.rate > 0]
        total = sum(b.rate for b in pool)
        if not pool or total <= 0 or remaining <= 0:
            break
        handed = 0
        for bk in pool:
            give = min(int(remaining * bk.rate / total), bk.size - bk.share)
            give -= give % 8192                      # whole blocks; the kernels assume it
            bk.share += give
            handed += give
        remaining -= handed
        if handed <= 0:
            break
    return model_bytes - remaining


def run_pass(link, nodes, batch):
    """Issue to every node, then collect -- and the instant a node answers, give it its next bank.

    A node with four banks must not wait for the rest of the fleet between them, and no node may ever be
    idle while it still has work. That is the whole difference between fifteen processors and a queue."""
    for nd in nodes:
        nd.queue = [b for b in nd.banks if b.share > 0]
        nd.busy = False
    outstanding = {}
    total = 0
    t0 = time.time()

    def kick(nd):
        if nd.busy or not nd.queue:
            return
        bk = nd.queue.pop(0)
        select(link, bk)
        arg = " %d" % batch if nd.kind == "teensy" and batch > 1 else ""
        link.send("@%d M 0 %d 1%s" % (nd.addr, bk.share, arg))
        outstanding[nd.addr] = bk
        nd.busy = True

    for nd in nodes:
        kick(nd)

    deadline = time.time() + 1800.0
    while outstanding and time.time() < deadline:
        t = link.line(max(1.0, deadline - time.time()))
        if t is None:
            break
        if t[:1].upper() != "M":
            continue
        try:
            total += int(t.split()[1])
        except (IndexError, ValueError):
            continue
        done = next((n for n in nodes if n.addr in outstanding and n.busy), None)
        if done is None:
            continue
        outstanding.pop(done.addr, None)
        done.busy = False
        kick(done)

    return time.time() - t0, total, len(outstanding)


def main():
    mb = float(sys.argv[1]) if len(sys.argv) > 1 else 32.0
    batch = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    model = int(mb * 1e6)

    def say(m):
        print(m)
        sys.stdout.flush()

    say("bench one -- the whole fleet")
    link = Link(PORT, BAUD)

    nodes = identify(link, say, enumerate_fleet(link, say))
    if not nodes:
        sys.exit("nothing answered. Check the head link, and that exactly one process holds the port.")

    say(NL + "  probing every Teensy for chips")
    for nd in nodes:
        if nd.kind == "teensy":
            probe_teensy_banks(link, say, nd)

    say(NL + "  qualifying and timing every bank")
    banks = []
    for nd in nodes:
        for bk in list(nd.banks):
            if qualify_bank(link, say, bk) and measure(link, say, bk):
                banks.append(bk)
            else:
                nd.banks.remove(bk)
    if not banks:
        sys.exit("no bank in the fleet returned a repeatable answer")

    say(NL + "    bank      kind      MB/s")
    for bk in sorted(banks, key=lambda b: -b.rate):
        say("    %-8s  %-8s  %6.2f" % (bk.name, bk.node.kind, bk.rate))

    held = allocate(banks, model, say)
    say("    bank      MB/s   share MB   seconds")
    for bk in sorted(banks, key=lambda b: -b.rate):
        if bk.share:
            say("    %-8s  %6.2f   %8.1f   %7.2f"
                % (bk.name, bk.rate, bk.share / 1e6, bk.share / 1e6 / bk.rate))
    if held < model:
        say("    %.1f MB of the model does not fit; the fleet holds %.1f MB"
            % ((model - held) / 1e6, held / 1e6))

    say(NL + "  loading weights")
    for bk in banks:
        if bk.share:
            select(link, bk)
            link.ask("@%d F 0 %d" % (bk.node.addr, bk.share), "F", timeout=1800.0)

    say(NL + "  %.1f MB = %.0f million parameters at one bit a weight"
        % (held / 1e6, held * 8 / 1e6))
    say(NL + "  round   wall s   MB/s    MMAC/s   tokens/s   missing")
    best = 0.0
    for r in range(rounds):
        wall, total, missing = run_pass(link, nodes, batch)
        if wall <= 0:
            continue
        mbps = held / wall / 1e6
        mmac = held * 8.0 * batch / wall / 1e6
        best = max(best, mmac)
        say("  %5d   %6.2f   %5.2f   %7.1f   %8.3f   %d"
            % (r, wall, mbps, mmac, batch / wall, missing))

    say(NL + "  best %.1f MMAC/s over %.0f million parameters" % (best, held * 8 / 1e6))
    link.close()


if __name__ == "__main__":
    main()
