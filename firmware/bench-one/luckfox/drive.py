#!/usr/bin/env python3
"""
drive.py -- the Luckfox owns everything except the arithmetic.

THE DIVISION OF LABOUR
----------------------
    the Teensy      64 MB of weights on a bus it drives itself, and a multiply-accumulate kernel at
                    2.07 cycles a weight. It takes orders and returns numbers.
    this board      a Cortex-A7 with NEON, 33 MB of DDR2 at roughly 930 MB/s, and an operating system.
                    It finds the banks, chooses their bus mode and timing, issues the work, checks the
                    answers and keeps the log.

Every decision lives here. Which selects exist, whether a bank runs four bits to the clock or one, how
fast it can be clocked, how long a burst may be before chip select has been low past the refresh window,
what to measure and what counts as correct. The Teensy has no opinion about any of those, which is the
point: changing a policy means editing this file rather than reflashing a microcontroller, and a worker
that holds no policy cannot drift between runs.

WHY THE WEIGHTS NEVER CROSS THE WIRE
------------------------------------
A megabyte at 1 Mbaud is eight seconds; the same megabyte out of PSRAM is a sixth of a second. So this
end sends the RULE that generates the weights, one F command, and the Teensy materialises them at bus
speed. The link carries commands and answers only, which is why its latency shows up as a constant
rather than as a bandwidth ceiling.

QUANTISATION IS THE LEVER
-------------------------
Nothing aimed at the clock, the burst or the instruction count has moved the aggregate. It will not: the
bus moves BYTES, and a byte costs the same whatever is packed into it. So the measurement that matters
is weights per byte.

    4 bits     2 weights a byte     the baseline
    2 bits     4 weights a byte     the same bytes, twice the weights
    1 bit      8 weights a byte     the same bytes again, twice again

All three kernels cost the same 1.25 instructions a weight, so at a bus-limited rate each halving of the
weight is a doubling of the multiply-accumulates a second. This measures that instead of asserting it, and
checks every kernel against arithmetic done here in Python before believing any number.

TWO BUS MODES, CHOSEN PER BANK
------------------------------
Six of the eight chips take a quad write perfectly and return wrong nibbles to a quad read -- errors
spread evenly across all four data lines, shuffling between runs, unchanged by the clock from 10.7 MHz
down to 2.3, by bursts from 8 bytes to 64, by the wait-cycle count, the sample instant, the bus keeper,
the hysteresis or the drive strength. Every one of them reads correctly when the chip drives ONE line
instead of four. So each bank is offered quad first and falls back to single-bit, which costs a quarter
of the bits a clock and is the difference between 16 MB of usable memory and 56.

    ./drive.py [kilobytes-per-bank] [rounds]

Link: UART3 at /dev/ttyS3, 1 Mbaud, to the Teensy's Serial2 on pins 7 and 8.
"""

import os
import sys
import time

NL = chr(10)

try:
    import serial
except ImportError:
    sys.exit("pyserial missing. It ships with the stock Luckfox image; check the PATH.")

PORT = "/dev/ttyS3"
BAUD = 1000000
# THE LOG GOES ON THE SD CARD, NOT ON THE ROOT FILESYSTEM.
#
# Root here is 67 MB of UBI flash. A verbose run writes about 300 kB, and thirty-odd runs filled it to
# 100% and killed a benchmark mid-round with ENOSPC -- which looked like a crash in the driver and was
# actually my own logging eating the operating system. The SD card is 3.7 GB with nothing on it.
def _logdir():
    for d in ("/mnt/sdcard/bench-logs", "/root/bench-logs"):
        try:
            if not os.path.isdir(d):
                os.makedirs(d)
            probe = os.path.join(d, ".w")
            with open(probe, "w") as f:
                f.write("x")
            os.remove(probe)
            return d
        except OSError:
            continue
    return "/tmp"


LOGDIR = None                    # resolved at startup by _logdir()

# The worker's timing table, in the worker's order: no-op counts inside the nibble loop, so a bigger
# number is a slower clock. Everything past index 9 exists only to have proved that slowing down is not
# the answer to a bank that will not read.
# Odd values are in here deliberately. With steps of two the sweep had to take whatever entry happened
# to fall inside a bank's window and paid for the distance to its edge, and that showed up as a 5% swing
# on the quad banks from nothing but a code-layout change elsewhere in the file. Filling in the gaps lets
# the sweep express where the edge actually is.
# The diagnostic tail of 32 to 128 no-ops is gone from the worker. It proved that slowing down does not
# rescue a bank that will not read, and it cost 25 template instantiations -- which on this target is a
# timing change, because GCC stops unrolling the payload loop when the file grows and the unrolled form
# is 21% faster. Every entry in this table is paid for in the speed of every other one.
NOPS = [4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24]
USABLE = 15

# tCEM, MEASURED RATHER THAN QUOTED.
#
# The PSRAM is DRAM inside and refreshes only while chip select is HIGH, so a burst is bounded by how
# long it holds chip select low and not by anything about the clock. The datasheet says 8 us and this
# bench used 7 for margin -- a number nothing here had ever tested, and an expensive one: at burst 16 a
# single-bit read spends 40 clocks on command, address and wait for every 128 clocks of payload, so a
# fifth of the bus is gone before a byte moves.
#
# It was tested the only way a refresh limit can be. A refresh violation does not fail on the pass that
# commits it; it fails later, when a row that was not refreshed in time is read. So each burst was filled
# once and read back eight times without rewriting, 8 MB per setting, on a quad bank and on two
# single-bit ones:
#
#     burst 16   6.1 us    0 wrong        burst 48   15.8 us   0 wrong
#     burst 24   8.7 us    0 wrong        burst 64   20.5 us   0 wrong
#     burst 32  11.1 us    0 wrong        burst 96   29.9 us   0 wrong
#
# Clean to 29.9 us, nearly four times the quoted limit. 21 us is taken as the budget, which selects the
# 20.5 us point and keeps the 29.9 us result as headroom above it rather than spending it.
#
# This is still the number that bit back once: slowing the clock LENGTHENS chip-select-low, so a bank
# that errors does not improve by going slower unless the burst shrinks with it. Both ends of that trade
# are computed here, together, from one measurement.
CS_LOW_BUDGET_US = 21.0          # overridden by the third command-line argument

# Inside the refresh window at every setting in the table, in both directions and both bus modes, so a
# slow candidate can never fail because the sweep lengthened chip-select-low.
SAFE_BURST = 16

# Chip-select setup: no-ops between select falling and the first clock edge. Asserting select and
# clocking immediately leaves the 74LVC138A about three to seven nanoseconds to propagate an enable it
# is specified to take up to six for. The two chips soldered to the Teensy have no decoder in the path
# and do not care. 16 no-ops is 27 nanoseconds, so it is tried second and only kept if it earns it.
SETUPS = [0, 16]

# Three ways to read a byte, and which is fastest is a property of the individual chip.
#
# Quad is four bits to the clock and always wins where it works, so it is tried first and nothing else
# is tried after it succeeds. The two single-bit modes differ only in the command: 0x03 is specified to
# 33 MHz and has no wait cycles, 0x0B adds eight wait cycles after the address so the clock may go
# faster. Measured over 256 kB, 0x0B is worth 1.19 to 1.21x on Y1, Y3 and Y5 and 0.95x on Y0 and Y2 --
# the eight extra clocks cost more than the clock gained when the clock does not actually improve. So
# both are measured per bank and each bank keeps its own winner. Taking the first mode that merely works
# would hand three banks a 20% loss.
MODES = [(0, "quad"), (1, "single 0x03"), (2, "single 0x0B")]

# The weight widths to measure, widest first so the comparisons read as gains.
WIDTHS = (4, 2, 1)

# HOW MANY TOKENS ONE TRIP ACROSS THE BUS CARRIES.
#
# At batch one every token pays for a full pass over the weights, and that pass is essentially the whole
# cost of this machine. But the banks are not compute-bound: a byte takes about 185 cycles to arrive on a
# single-bit bank and the one-bit kernel spends about ten on it, which is why the 1-bit kernel does four
# times the multiply-accumulates of the 4-bit one for six percent more time.
#
# So the weights are read once into tightly-coupled memory and scored against BATCH activation vectors
# out of it. The bus does identical work and the answer count multiplies. Measured on the hardware, at
# batch 32 the quad bank reaches 4.54x its single-token rate and the single-bit bank keeps climbing past
# that, because its bus shadow is deeper. Batching only applies at one bit, where the kernel and the
# tables exist for it.
BATCH = 1
WAITS_0B = 8                    # the datasheet value for 0x0B; swept and confirmed on the hardware
WAITS_QUAD = 6                  # the datasheet value for 0xEB, and the only one the good banks accept

# Pad configuration for the four data lines, and this one was a real bug for a long time.
#
# 2 is a Schmitt input plus fast slew plus full pad bandwidth. pinMode(OUTPUT) writes drive strength and
# nothing else, so every read before this used a plain threshold, which is the worst input for a weak
# driver at the end of ribbon: one slow edge crosses a fixed threshold several times and reads as several
# transitions. Measured over 256 kB on the single-bit path it took Y5 from completely broken to zero wrong
# bytes and Y0 from 297 to zero, with no bank made worse.
#
# An earlier test reported this as making no difference. That test put the bandwidth field on top of the
# drive-strength field, so it ran with the pad crippled and its answer meant nothing.
PAD_MODE = 2

SEL_CS0, SEL_CS1, SEL_DEC = 0, 1, 2
ROUTES = [(SEL_CS0, 0, "CS0"), (SEL_CS1, 0, "CS1")] + \
         [(SEL_DEC, y, "Y%d" % y) for y in range(8)]


class Link(object):
    def __init__(self, port, baud, log, verbose=True):
        self.ser = serial.Serial(port, baud, timeout=0.5)
        self.log = log
        self.verbose = verbose
        time.sleep(0.2)
        # DRAIN WHATEVER IS STILL IN FLIGHT BEFORE ASKING ANYTHING.
        #
        # reset_input_buffer clears what this end has already received; it does nothing about bytes the
        # Teensy is still transmitting. A diagnostic script that exits without reading its last reply
        # leaves those in the wire, and then the first command of the next run consumes somebody else's
        # answer and reports the board as dead. That cost a run and looked like a hardware failure.
        quiet = 0
        while quiet < 3:
            self.ser.timeout = 0.1
            if self.ser.read(4096):
                quiet = 0
            else:
                quiet += 1
        self.ser.reset_input_buffer()

    def ask(self, cmd, timeout=120.0):
        """One line out, one line back. Neither end keeps state between requests, so a reply that
        arrives late cannot be mistaken for the answer to the next question.

        read_until stops at the newline. A plain read(n) blocks for the whole timeout unless n bytes
        arrive, which made every exchange look like it cost half a second -- a measurement of the
        timeout argument rather than of the link."""
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()
        t0 = time.time()
        deadline = t0 + timeout
        buf = b""
        want = cmd[0].upper()
        while time.time() < deadline:
            self.ser.timeout = min(0.5, max(0.05, deadline - time.time()))
            buf += self.ser.read_until(b"\n")
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                line = line.decode(errors="replace").strip()
                if line[:1].upper() != want:
                    continue
                if self.verbose:
                    self.log("    %-26s -> %-36s %6.1f ms"
                             % (cmd, line, (time.time() - t0) * 1000.0))
                return line
        self.log("    %-26s -> TIMEOUT after %.1f s" % (cmd, timeout))
        return None

    def close(self):
        self.ser.close()


# ------------------------------------------------------------------------------------------------
#  the weights, and what the answer should be
#
#  Both kernels are checked against arithmetic done here before any throughput number is reported. A
#  2-bit kernel that is fast and wrong is worth nothing, and the only way to know which it is without
#  trusting the board that computed it is to compute it somewhere else.
# ------------------------------------------------------------------------------------------------
def pattern(addr, n):
    return bytearray(((addr + i) * 0x9D + 0x3B) & 0xFF for i in range(n))


def activations():
    return [((i * 37) & 0x7F) - 64 for i in range(2048)]


def reference_mac(w, bits, xv):
    """What the kernel must produce for these bytes.

    4-bit: byte j holds weights 2j (low nibble) and 2j+1 (high nibble), zero point 8.
    2-bit: byte j holds weights 4j..4j+3, field f at bit 2f, zero point 2.
    1-bit: byte j holds weights 8j..8j+7, bit f, and a bit means -1 or +1 rather than 0 or 1.
    The activation index wraps every 2048 weights -- every 1024 bytes at 4 bits, 512 at 2 and 256 at 1 --
    and all three divide the 8 kB block, so no chunk boundary lands mid-wrap."""
    total = 0
    if bits == 4:
        for j, b in enumerate(w):
            total += ((b & 0x0F) - 8) * xv[(2 * j) % 2048]
            total += ((b >> 4) - 8) * xv[(2 * j + 1) % 2048]
    elif bits == 2:
        for j, b in enumerate(w):
            for f in range(4):
                total += (((b >> (2 * f)) & 3) - 2) * xv[(4 * j + f) % 2048]
    else:
        for j, b in enumerate(w):
            for f in range(8):
                total += (2 * ((b >> f) & 1) - 1) * xv[(8 * j + f) % 2048]
    total &= 0xFFFFFFFF                       # the kernel accumulates in int32 and wraps
    return total - 0x100000000 if total >= 0x80000000 else total


# ------------------------------------------------------------------------------------------------
#  the decisions
# ------------------------------------------------------------------------------------------------
def discover(link, say):
    """Ask all ten possible selects who is there. A real ESP-PSRAM64H answers 0x0D then 0x5D and an open
    bus cannot, so this separates a chip from a wire."""
    say("\n  discovery: asking all ten selects for an identity")
    found = []
    for kind, y, name in ROUTES:
        r = link.ask("P %d %d" % (kind, y), timeout=30.0)
        if not r or not r.startswith("P "):
            say("    %-4s no reply" % name)
            continue
        f = r.split()
        ok = f[1] == "1"
        say("    %-4s %s  id %s %s" % (name, "PRESENT" if ok else "absent ", f[2], f[3]))
        if ok:
            found.append((kind, y, name))
    say("  %d banks present" % len(found))
    return found


# Which banks have already been put into which bus mode.
ARMED = {}

# Does every bank switch reset the chip, or only the first one?
#
# Removing the per-switch reset was tried and made the drift WORSE -- 7 rounds in 7 instead of 1 in 5.
# The reset sequence carries a 2 ms delay with chip select high, and that delay was the only substantial
# refresh window in the whole run. With an explicit inter-burst gap there is a proper one, so this is
# left switchable and measured rather than assumed either way.
RESET_EVERY_SWITCH = True

# Chip-select HIGH no-ops between bursts: the window in which the chip refreshes. Swept from 0 to 400
# and it changed nothing about the drift while costing up to half the throughput, so refresh starvation
# is not what was wrong. Left at 0 and left switchable.
GAP_NOPS = 0

# How many times a chosen setting must read the span back cleanly from one fill before it is accepted.
# One was not enough and that is what let marginal settings into the benchmark.
CONFIRM_PASSES = 4


def select(link, kind, y, setting, rearm=False):
    """Put a bank in force, and reset it only when its mode has to change.

    B sets the bus mode, and setting the mode means sending 0xF5, 0x66, 0x99 and possibly 0x35 -- a quad
    exit and a reset. That is right once. Doing it on every bank switch is not, and it was corrupting
    the measurement: each bank read the same megabyte twenty times with one distinct answer, 140 MB with
    no drift at all, while the seven-bank benchmark returned a different sum about one round in six. The
    benchmark switches seven banks twice a round, so a six-round run was sending eighty-four resets, and
    a reset arriving while the chip is mid-refresh can cost a row.

    E changes the route and touches nothing else. The mode is established once with B and every
    selection after that is an E."""
    mode, wi, ri, nb, su = setting[:5]
    link.ask("Q %d" % mode, timeout=30.0)
    # EVERY KNOB IS SET HERE, EVERY TIME, INCLUDING THE ONES THAT ARE NOT BEING SWEPT.
    #
    # The quad wait count was left to the worker's default and never sent, which made it the one piece of
    # policy the host did not own -- so a diagnostic script that set it to 96 and exited left it there,
    # and the next benchmark run silently failed both quad banks into single-bit mode and reported 26.39
    # MMAC/s instead of 31.12. The whole point of this split is that the worker remembers nothing the host
    # did not tell it, and a default is something it remembers.
    link.ask("D %d" % WAITS_QUAD, timeout=30.0)
    link.ask("K %d" % PAD_MODE, timeout=30.0)
    if mode == 2:
        link.ask("J %d" % WAITS_0B, timeout=30.0)
    link.ask("U %d" % su, timeout=30.0)
    link.ask("Y %d" % GAP_NOPS, timeout=30.0)
    key = (kind, y)
    if rearm or RESET_EVERY_SWITCH or ARMED.get(key) != mode:
        link.ask("B %d %d" % (kind, y), timeout=30.0)
        ARMED[key] = mode
    else:
        link.ask("E %d %d" % (kind, y), timeout=30.0)
    link.ask("K %d" % PAD_MODE, timeout=30.0)      # B and E both go through pinMode, which rewrites it
    link.ask("T %d %d %d" % (wi, ri, nb), timeout=30.0)


def burst_bound(link, letter):
    """Two timed bursts give the fixed overhead and the per-byte cost, so the longest burst that keeps
    chip select inside the refresh window is arithmetic rather than a sweep."""
    a = link.ask("%s 96" % letter, timeout=30.0)
    b = link.ask("%s 32" % letter, timeout=30.0)
    if not a or not b:
        return SAFE_BURST, 0.0, 0.0
    c96, c32 = int(a.split()[2]), int(b.split()[2])
    per = (c96 - c32) / 64.0
    fix = c96 - per * 96.0
    if per <= 0:
        return SAFE_BURST, fix, per
    n = int((CS_LOW_BUDGET_US * 1e-6 * FCPU - fix) / per)
    return max(8, min(96, (n // 8) * 8)), fix, per


def pick_burst(link, say):
    """BOTH directions, and the smaller wins.

    Computing it from the read alone and using it for the write as well is the mistake that cost the
    first run six banks: at a slow write setting the write is the slower direction per byte, so a burst
    comfortable for the read holds chip select low past the refresh window on the write. The data never
    lands, and then every read setting looks broken because there was nothing correct to read."""
    nr, fr, pr = burst_bound(link, "R")
    nw, fw, pw = burst_bound(link, "W")
    n = min(nr, nw)
    say("        burst: read %.0f+%.1f/byte -> %d, write %.0f+%.1f/byte -> %d, taking %d"
        % (fr, pr, nr, fw, pw, nw, n))
    return n


def sweep(link, say, kind, y, mode, setup, probe):
    """The fastest write and read this bank verifies at, at a burst that cannot be the reason.

    The read is qualified first against a slow write, because a bad write poisons every read after it
    and the blame then lands in the wrong place. Then the write is pushed against the read just found."""
    WHOLD = 11                                  # 16 no-ops: generous, and inside tCEM at burst 16
    select(link, kind, y, (mode, WHOLD, USABLE - 1, SAFE_BURST, setup), rearm=True)

    ri_ok = None
    for ri in range(USABLE):
        link.ask("T %d %d %d" % (WHOLD, ri, SAFE_BURST), timeout=30.0)
        if not link.ask("F 0 %d" % probe, timeout=300.0):
            continue
        v = link.ask("V 0 %d" % probe, timeout=300.0)
        if v and v.startswith("V ") and int(v.split()[1]) == 0:
            ri_ok = ri
            break
    if ri_ok is None:
        return None

    wi_ok = WHOLD
    for wi in range(USABLE):
        link.ask("T %d %d %d" % (wi, ri_ok, SAFE_BURST), timeout=30.0)
        if not link.ask("F 0 %d" % probe, timeout=300.0):
            continue
        v = link.ask("V 0 %d" % probe, timeout=300.0)
        if v and v.startswith("V ") and int(v.split()[1]) == 0:
            wi_ok = wi
            break

    link.ask("T %d %d %d" % (wi_ok, ri_ok, SAFE_BURST), timeout=30.0)
    nb = pick_burst(link, say)
    return (mode, wi_ok, ri_ok, nb, setup)


def confirm(link, say, kind, y, setting, span):
    """A SWEEP IS NOT A QUALIFICATION.

    Thirty-two kilobytes of silence bounds the error rate below roughly one in thirty thousand, and the
    fault that bit this project ran at about one in six hundred thousand. The sweep finds the shape of
    the cliff; only a full pass says where to stand. So the chosen setting has to survive the span the
    benchmark will really read, and is stepped back until it does -- read first, then write, with the
    burst recomputed after every step, because slowing down lengthens chip-select-low."""
    mode, wi, ri, nb, su = setting
    for _ in range(8):
        select(link, kind, y, (mode, wi, ri, nb, su))
        link.ask("F 0 %d" % span, timeout=900.0)

        # ONE CLEAN PASS IS NOT A QUALIFICATION EITHER.
        #
        # This used to accept the first setting that read a full span back without error, and that is how
        # marginal settings got through: the benchmark then returned a different answer about one round
        # in six. Reading the same megabyte CONFIRM_PASSES times from one fill costs almost nothing next
        # to the sweep that precedes it and rejects the settings that only worked once. It is also a
        # retention test, because the data is not rewritten between passes.
        bad = -1
        rate = 0.0
        for k in range(CONFIRM_PASSES):
            v = link.ask("V 0 %d" % span, timeout=900.0)
            bad = int(v.split()[1]) if v and v.startswith("V ") else -1
            if k == 0 and bad == 0:
                rate = span / (int(v.split()[2]) / FCPU) / 1e6
            if bad != 0:
                break
        if bad == 0:
            return (mode, wi, ri, nb, su, rate)
        say("        %d wrong over %d kB at write %d read %d burst %d"
            % (bad, span // 1024, NOPS[wi], NOPS[ri], nb))
        if ri + 1 < USABLE:
            ri += 1
        elif wi + 1 < USABLE:
            wi += 1
        else:
            return None
        link.ask("T %d %d %d" % (wi, ri, SAFE_BURST), timeout=30.0)
        nb = pick_burst(link, say)
    return None


def qualify(link, say, kind, y, name, span, probe=32768):
    """Every mode that survives the full span, and the fastest of them wins.

    Quad short-circuits because four bits to the clock cannot lose to one, so a bank that reads in quad
    is not asked anything further. The two single-bit modes are both measured, because which of them is
    faster depends on whether the chip will actually take a faster clock with the wait cycles -- three
    of the five do and two do not, and picking the first that merely works costs the three 20%."""
    say("    %s" % name)
    found = []
    for mode, mlabel in MODES:
        got = None
        for su in SETUPS:
            s = sweep(link, say, kind, y, mode, su, probe)
            if not s:
                continue
            c = confirm(link, say, kind, y, s, span)
            if c:
                got = c
                break
        if got:
            say("      %s: %-11s write %2d read %2d burst %2d setup %2d -> %5.2f MB/s"
                % (name, mlabel, NOPS[got[1]], NOPS[got[2]], got[3], got[4], got[5]))
            found.append((got[5], mode, got))
            if mode == 0:
                break
        else:
            say("      %s: no %s setting survives %d kB" % (name, mlabel, span // 1024))
    if not found:
        return None
    found.sort(reverse=True)
    best = found[0][2]
    if len(found) > 1:
        say("      %s: keeping %s at %.2f MB/s"
            % (name, MODES[best[0]][1], best[5]))
    return best[:5]


def stable(link, say, kind, y, name, setting, span, reps=6):
    """A BANK THAT CANNOT GIVE THE SAME ANSWER TWICE IS NOT A USABLE BANK.

    Qualification asks whether a setting reads a span back without wrong bytes, four times from one fill.
    That is necessary and it is not sufficient: Y4 passes it and then changes its answer in the benchmark,
    which is how a chip that failed every mode last night came to be counted as working memory this
    morning.

    So each bank is asked for the same number several times and has to give it. The partial sum is a free
    checksum over every byte in the span, so this costs one extra read per repetition and nothing else.
    A bank that fails is left out, and the reported rate is then a rate over banks that demonstrably
    return what was written to them."""
    select(link, kind, y, setting)
    link.ask("F 0 %d" % span, timeout=900.0)
    answers = {}
    for _ in range(reps):
        m = link.ask("M 0 %d 4" % span, timeout=900.0)
        if not m or not m.startswith("M "):
            return False
        v = int(m.split()[1])
        answers[v] = answers.get(v, 0) + 1
    if len(answers) == 1:
        return True
    common = max(answers, key=lambda k: answers[k])
    say("      %s gave %d different answers in %d reads (%s); leaving it out"
        % (name, len(answers), reps,
           ", ".join("%+d" % (v - common) for v in answers if v != common)))
    return False


def check_kernels(link, say, xv):
    """Prove both kernels before trusting either throughput figure. One 8 kB block, filled with the
    known pattern, computed here in Python and there in assembly. Integer arithmetic, so a correct
    kernel is bit-identical and there is no tolerance to argue about."""
    say("\n  checking both kernels against arithmetic done on this board")
    n = 8192
    w = pattern(0, n)
    ok = True
    for bits in WIDTHS:
        want = reference_mac(w, bits, xv)
        r = link.ask("M 0 %d %d" % (n, bits), timeout=300.0)
        got = int(r.split()[1]) if r and r.startswith("M ") else None
        good = (got == want)
        ok = ok and good
        say("    %d-bit: teensy %-12s python %-12s %s"
            % (bits, got, want, "MATCH" if good else "MISMATCH"))
    return ok


def bench(link, say, banks, settings, span, rounds):
    say("\n  %d kB a bank, %.1f MB a pass, %d rounds"
        % (span // 1024, span * len(banks) / 1e6, rounds))
    say("\n  round  bits   partial sum       teensy ms     MB/s    MMAC/s    link ms   answer")
    best = dict((b, 0.0) for b in WIDTHS)
    per_bank = {}
    seen = {}
    drift = dict((b, 0) for b in WIDTHS)
    for r in range(rounds):
        for bits in WIDTHS:
            total_cyc = 0
            total_bytes = 0
            partial = 0
            wall = 0.0
            for (kind, y, name) in banks:
                select(link, kind, y, settings[name])
                t0 = time.time()
                nb_arg = (" %d" % BATCH) if (bits == 1 and BATCH > 1) else ""
                m = link.ask("M 0 %d %d%s" % (span, bits, nb_arg), timeout=900.0)
                wall += time.time() - t0
                if not m or not m.startswith("M "):
                    continue
                f = m.split()
                partial += int(f[1])
                cyc = int(f[2])
                total_cyc += cyc
                total_bytes += span
                if r == 0:
                    per_bank.setdefault(name, {})[bits] = span / (cyc / FCPU) / 1e6
            if total_cyc == 0:
                continue
            secs = total_cyc / FCPU
            mbps = total_bytes / secs / 1e6
            per_byte = (8.0 / bits) * (BATCH if bits == 1 else 1)
            macs = total_bytes * per_byte / secs / 1e6
            if bits not in seen:
                seen[bits] = partial
                mark = "first"
            elif partial == seen[bits]:
                mark = "same"
            else:
                mark = "CHANGED by %d" % (partial - seen[bits])
                drift[bits] += 1
            best[bits] = max(best[bits], macs)
            say("  %5d  %4d   %-15d   %8.1f   %6.2f   %7.2f    %6.1f   %s"
                % (r, bits, partial, secs * 1000.0, mbps, macs,
                   wall * 1000.0 - secs * 1000.0, mark))
    if any(drift.values()):
        say("  of %d repeat rounds, answers changed: %s"
            % (rounds - 1, ", ".join("%d at %d bits" % (drift[b], b) for b in WIDTHS)))
        say("  The rates above are throughput over bytes that are not the bytes that were written.")
    else:
        say("  every round returned the same answer at every width")
    return best, per_bank, drift


def main():
    global FCPU
    global CS_LOW_BUDGET_US, GAP_NOPS, RESET_EVERY_SWITCH
    kb = int(sys.argv[1]) if len(sys.argv) > 1 else 1024
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    if len(sys.argv) > 3:
        CS_LOW_BUDGET_US = float(sys.argv[3])
    if len(sys.argv) > 4:
        GAP_NOPS = int(sys.argv[4])
    if len(sys.argv) > 5:
        RESET_EVERY_SWITCH = bool(int(sys.argv[5]))
    if len(sys.argv) > 6:
        globals()["BATCH"] = int(sys.argv[6])
    span = kb * 1024
    span -= span % 8192                       # whole 8 kB blocks; the kernels assume it
    span = max(8192, span)

    logdir = _logdir()
    logpath = os.path.join(logdir, "drive-%s.log" % time.strftime("%Y%m%d-%H%M%S"))
    logfile = open(logpath, "w")

    def say(msg):
        print(msg)
        sys.stdout.flush()
        logfile.write(msg + "\n")
        logfile.flush()

    say("bench one -- the Luckfox driving the Teensy")
    say("  chip-select budget %.1f us, inter-burst gap %d no-ops, reset every switch %s, batch %d"
        % (CS_LOW_BUDGET_US, GAP_NOPS, RESET_EVERY_SWITCH, BATCH))
    say("  log: %s" % logpath)

    link = Link(PORT, BAUD, say)
    info = link.ask("I", timeout=60.0)
    if not info or not info.startswith("I "):
        sys.exit("no reply on %s. Check the two signal wires and that both boards share a ground."
                 % PORT)
    say("  teensy: %s" % info)
    f = info.split()
    FCPU = float(f[f.index("fcpu") + 1])

    banks = discover(link, say)
    if not banks:
        sys.exit("no banks answered")

    say("\n  qualifying: quad first, then single-bit, each confirmed over the full span")
    settings = {}
    live = []
    for (kind, y, name) in banks:
        s = qualify(link, say, kind, y, name, span)
        if s:
            settings[name] = s
            live.append((kind, y, name))

    dead = [n for (_, _, n) in banks if n not in settings]
    if dead:
        say("\n  not usable: %s" % ", ".join(dead))
    if not live:
        sys.exit("no bank held a setting over the full span")

    say("\n  stability: every bank has to return the same answer six times over")
    keep = []
    for (kind, y, name) in live:
        if stable(link, say, kind, y, name, settings[name], span):
            keep.append((kind, y, name))
    dropped = [n for (_, _, n) in live if n not in [k[2] for k in keep]]
    if dropped:
        say("  dropped for an unstable answer: %s" % ", ".join(dropped))
    live = keep
    if not live:
        sys.exit("no bank returned a repeatable answer")

    nq = sum(1 for (_, _, n) in live if settings[n][0] == 0)
    say("  usable: %d banks, %.0f MB -- %d quad, %d single-bit"
        % (len(live), len(live) * 8.0, nq, len(live) - nq))

    # Hand the chosen settings to whatever runs next.
    #
    # Qualification is the expensive, hardened part of this driver and there is no reason for a second
    # program to repeat it. One line a bank: route, bus mode, timing, burst, chip-select setup.
    with open("/root/banks.txt", "w") as bf:
        for (kind, y, name) in live:
            mode, wi, ri, nb, su = settings[name][:5]
            bf.write("%d %d %s %d %d %d %d %d%s"
                     % (kind, y, name, mode, wi, ri, nb, su, NL))
    say("  settings written to /root/banks.txt for the concurrent runner")

    xv = activations()
    kind, y, name = live[0]
    select(link, kind, y, settings[name])
    link.ask("F 0 8192", timeout=300.0)
    kernels_ok = check_kernels(link, say, xv)

    say("\n  loading weights")
    for (kind, y, name) in live:
        select(link, kind, y, settings[name])
        r = link.ask("F 0 %d" % span, timeout=900.0)
        if r and r.startswith("F "):
            say("    %-4s %5.2f MB/s write" % (name, span / (int(r.split()[1]) / FCPU) / 1e6))

    best, per_bank, drift = bench(link, say, live, settings, span, rounds)

    say("\n  per bank, read plus multiply-accumulate")
    say("    bank   mode         " + "".join("%d-bit MB/s  " % b for b in WIDTHS))
    for (kind, y, name) in live:
        p = per_bank.get(name, {})
        say("    %-5s  %-11s  " % (name, MODES[settings[name][0]][1])
            + "".join("%10.2f   " % p.get(b, 0.0) for b in WIDTHS))

    # WHERE TO PUT A MODEL, GIVEN BANKS THAT ARE NOT THE SAME SPEED.
    #
    # Reading every bank equally gives the harmonic mean of their rates, and with two banks at 9.7 MB/s
    # and five at 2.59 the harmonic mean sits near the slow end. The banks are not interchangeable, so a
    # model smaller than the total capacity should not be spread evenly across them: it should fill the
    # fast ones first and stop. That is the same load-balance result this project already measured on the
    # render fleet, where equal shares wasted a third of the machine.
    rates = sorted(((per_bank.get(n, {}).get(4, 0.0), n) for (_, _, n) in live), reverse=True)
    say(NL + "  where to put a model, fastest bank first")
    say("    size MB   banks used   seconds a pass   effective MB/s")
    cum_t, cum_b, used = 0.0, 0.0, []
    for rate, nm in rates:
        if rate <= 0:
            continue
        used.append(nm)
        cum_b += 8.0
        cum_t += 8.0 / rate
        say("    %7.0f   %10d   %14.2f   %14.2f" % (cum_b, len(used), cum_t, cum_b / cum_t))
    say("    Spreading a model that would fit in the fast banks over all of them costs real time:")
    say("    a sequential pass is the SUM of the per-bank times, so the slow banks set its length.")

    say("\n  RESULT over %d banks, %.1f MB of weights a pass, %.0f MB addressable"
        % (len(live), span * len(live) / 1e6, len(live) * 8.0))
    for b in WIDTHS:
        say("    %d-bit  %7.2f MMAC/s%s"
            % (b, best[b],
               "" if b == WIDTHS[0] or best[WIDTHS[0]] <= 0
               else "   %.2fx the %d-bit rate over the same bytes"
                    % (best[b] / best[WIDTHS[0]], WIDTHS[0])))
    if BATCH > 1 and best.get(1, 0) > 0:
        secs_pass = sum(8.0 / per_bank.get(nm, {}).get(1, 1e9) for (_, _, nm) in live)
        say(NL + "  what that is as a model, at one bit a weight")
        say("    %d banks hold %.0f MB = %.0f million parameters"
            % (len(live), len(live) * 8.0, len(live) * 8.0 * 8))
        say("    one pass over all of them: %.2f s" % secs_pass)
        say("    at batch %d that pass yields %d tokens -> %.3f tokens/s"
            % (BATCH, BATCH, BATCH / secs_pass))
        say("    at batch 1 the same weights give %.3f tokens/s" % (1.0 / secs_pass))

    say("    kernels verified against this board's own arithmetic: %s"
        % ("yes" if kernels_ok else "NO -- the figures above are throughput, not results"))
    say("    answer identical in every round: %s"
        % ("yes" if not any(drift.values()) else
           "NO -- %d rounds drifted, so this configuration is not usable" % sum(drift.values())))
    link.close()
    logfile.close()


if __name__ == "__main__":
    main()
