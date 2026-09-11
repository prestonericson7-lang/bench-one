#!/usr/bin/env python3
"""
sim_stack.py -- run the whole BENCH ONE stack at the speeds the real parts actually deliver

WHAT THIS IS FOR
----------------
Not to produce a trained model. The weights this throws away. The point is to find out whether the
machine OPERATES AS INTENDED before anything is soldered: whether queries come back inside their
deadline, whether the fast tier carries the load and the slow tiers are only consulted when it is
unsure, whether learning actually improves recall, and whether a node stalling for its radio can
change an answer. Bench time is the expensive thing; this is where the design mistakes get found.

EVERY NODE RUNS AT ITS OWN SPEED
--------------------------------
A simulation where all nodes are equally fast tests nothing, because the whole architecture is a
response to them being wildly unequal. The table in SPEEDS below is the model. Two columns matter:
how many vectors a node holds, and how long it takes to compare one. Everything else follows.

MEASURED vs ESTIMATED -- read this before trusting a number
-----------------------------------------------------------
Only one figure here has been measured on silicon: the FPGA's logic cost, from yosys. The rest are
derived from clock rates, instruction costs and bus widths, and each is marked. They are stated so
they can be REPLACED, not so they can be believed. tests/system_test_teensy/ prints the real Teensy
figure; when you run it, put that number in the table and re-run this.

Timing is linear in the number of vectors a node scans, so behaviour is verified at whatever scale
Python can carry and the wall-clock figures scale exactly to full capacity. Both are reported.

RUN
    python sim_stack.py
    python sim_stack.py --vectors 40000 --queries 400
"""

import random
import sys

HD_BITS = 8192
HD_BYTES = HD_BITS // 8

# Distances between two independent random 8192-bit vectors cluster at 4096 with a standard
# deviation of sqrt(8192)/2 = 45.25. Six of those below the mean is the line between "this is
# something I know" and "this is noise".
RANDOM_MEAN = HD_BITS // 2
SIGMA = (HD_BITS ** 0.5) / 2.0
RECOGNISE = RANDOM_MEAN - 6 * SIGMA          # 3824

HD_NO_SLOT = 0xFFFF
HD_FAR = 0xFFFFFFFF


def majority(members, rng):
    """Bit-wise majority of up to 15 hypervectors, computed bit-parallel.

    A prototype is the majority of the instances it absorbs: near everything it was made from and
    near nothing else. Done naively this is 8192 Python iterations per member. Instead the per-bit
    counts are held as binary PLANES -- plane j carries bit j of every count at once -- so adding a
    member is a ripple carry across four whole-vector integer operations regardless of width.

    AN EVEN NUMBER OF MEMBERS NEEDS A TIE-BREAK, AND THIS IS NOT A DETAIL.
    With k = 2 there is no majority: every bit is either 2, 1 or 0 votes. The first version of this
    function required count >= k/2 + 1, which for k = 2 means BOTH bits set -- plain AND. AND of two
    random-dense vectors halves the density of ones, and everything in this representation depends on
    density staying near half: a sparse vector drifts toward every other sparse vector and away from
    the things it was built from. The simulation caught it immediately, reporting that merging two
    exemplars of the same concept made recognition WORSE, 889 to 1043.

    So ties are broken at random, per bit, which preserves density exactly.
    """
    k = len(members)
    planes = []
    for v in members:
        carry = v
        for j in range(len(planes)):
            planes[j], carry = planes[j] ^ carry, planes[j] & carry
            if not carry:
                break
        if carry:
            planes.append(carry)

    full = (1 << HD_BITS) - 1

    def count_is(val):
        m = full
        for j in range(len(planes)):
            m &= planes[j] if (val >> j) & 1 else ~planes[j]
        return m & full

    out = 0
    for val in range(k // 2 + 1, k + 1):        # a clear majority
        out |= count_is(val)
    if k % 2 == 0:                              # exactly half: coin flip, per bit
        out |= count_is(k // 2) & rng.getrandbits(HD_BITS)
    return out & full


def popcount(x):
    try:
        return x.bit_count()
    except AttributeError:
        return bin(x).count("1")


_BITS_IN_BYTE = [[b for b in range(8) if (v >> b) & 1] for v in range(256)]


def set_bits(x):
    """Positions of the set bits, without 8192 Python iterations."""
    out = []
    data = x.to_bytes(HD_BYTES, "little")
    for i, byte in enumerate(data):
        if byte:
            base = i * 8
            for b in _BITS_IN_BYTE[byte]:
                out.append(base + b)
    return out


# ---------------------------------------------------------------------------------------------
#  THE SPEED TABLE. This is the model of your hardware.
# ---------------------------------------------------------------------------------------------
#   count        how many of these you have
#   capacity     hypervectors the node can hold at 1 KB each
#   us_per_vec   microseconds to compare the query against one stored vector
#   stall_p      probability a given query finds this node unavailable
#   source       measured, or how the number was derived
SPEEDS = {
    "teensy": dict(
        count=9, capacity=16384, us_per_vec=31.34, stall_p=0.00,
        source="MEASURED on a Teensy 4.1 with two PSRAM chips soldered: 18,805 cycles per "
               "8192-bit compare out of PSRAM at 600 MHz. Internal RAM is 3.66 us, but only "
               "about a thousand vectors fit there. 16 MB holds 16,384.",
        index_bits=128, index_us=0.10, index_mem_b=18),
    # PSRAM costs almost nothing here, and that is the opposite of the Teensy. On the Teensy the
    # core is fast enough that memory dominates and PSRAM costs 8.6x. On the S3 the core is slow
    # enough that compute dominates and PSRAM costs 1.22x. So an S3 should hold its whole working
    # set in PSRAM without hesitation, where a Teensy must think about it.
    "esp32s3": dict(
        count=15, capacity=8192, us_per_vec=60.07, stall_p=0.05,
        source="MEASURED on an ESP32-S3 rev 2, 240 MHz, 8 MB octal PSRAM, heatsink: 46.07 "
               "cycles per 32-bit word (no popcount instruction on Xtensa LX7), 49.14 us per "
               "compare from internal SRAM and 60.07 us from PSRAM. The radio cost nothing "
               "measurable: 278,459 compares over 15 s with the stack retrying association, "
               "longest gap 0.55 ms, zero gaps past 1 ms -- Arduino runs on core 1 and the wifi "
               "stack on core 0. stall_p is kept at 5% because a ~1 s dropout has been seen in "
               "the field under a real association with traffic, which this did NOT reproduce.",
        index_bits=128, index_us=0.77, index_mem_b=18),
    # 33 MB TOTAL, NOT 64. The RV1103 carries 64 MB of DDR2 but the kernel only ever sees 33,
    # because Rockchip reserves the rest for the ISP and video engine before Linux boots. Measured
    # MemAvailable was 13,208 kB with the stock Buildroot image running, so ~13,000 hypervectors is
    # the hard ceiling and the capacity below leaves the process room to exist. The model said
    # 20,000, which was never possible.
    "luckfox": dict(
        count=10, capacity=10000, us_per_vec=1.00, stall_p=0.05,
        source="MEASURED over ADB on a Luckfox Pico Mini, Buildroot, kernel 5.10.110: "
               "33,560 kB MemTotal and 13,208 kB MemAvailable. CPU is a Cortex-A7 rev 5 with "
               "neon, vfpv4 and edsp, so bench_hdc's USAD8 path DOES compile here. "
               "us_per_vec is still an ESTIMATE -- it needs a cross-compiler, which the board "
               "does not have and this PC does not either. Also measured: no eth0. The on-die "
               "MAC and PHY are absent from the device tree, so the only network is USB RNDIS. "
               "        Scanned LINEARLY on purpose: 10,000 vectors is 10 ms against a 50 ms "
               "deadline, so an index would spend 703 KB of a 13 MB budget and add a refusal "
               "path to solve a problem this node does not have."),

    # The card is the same part in both hosts and behaves the same: a Teensy measured 1,486 us and
    # a Luckfox 1,294 us for a random 1 KB read. That agreement is worth something -- it says the
    # latency is the card, not the host, so this number carries to any node with a card in it.
    # The tail does not: a single fetch hit 34.4 ms, two thirds of a 50 ms budget, almost certainly
    # the card's own garbage collection. Anything reading a card must tolerate that.
    "luckfox_sd": dict(
        count=10, capacity=600000, us_per_vec=1294.1, stall_p=0.00,
        index_bits=128, index_us=0.03, index_mem_b=20,
        source="MEASURED on the Luckfox's 4 GB card: write 10.1 MB/s, sequential read 15.9 MB/s, "
               "random 1 KB read mean 1294.1 us / median 1265.3 / p99 1661.0 / WORST 34,445.5 us. "
               "        Capacity is set by the INDEX, not the card. 600,000 entries at 20 B is "
               "12 MB, which is all the DDR2 there is; the card itself holds 3.67M. The worst "
               "single fetch measured 34.4 ms, so the survivor cap has to stay low."),

    "psram": dict(
        count=6, capacity=81920, us_per_vec=330.0, stall_p=0.00,
        source="ESTIMATE: 1-bit SPI at 25 MHz is ~3.1 MB/s, so 1 KB takes 330 us. "
               "psram_bringup measures the real figure on a built stick.",
        index_bits=512, index_us=0.03, index_mem_b=72),
    # A card is NOT a random-access body store. Measured on a 4 GB card in a Teensy 4.1:
    # 1,486 us for a random 1 KB read against 17.1 MB/s sequential -- a 25x gap. An index in
    # PSRAM addressing 839,000 bodies on the card costs 921 ms a query, which is past even the
    # deep-tier deadline, and 90% of that is streaming the index, not touching the card.
    #
    # The leverage is the sequential figure. Group similar vectors into contiguous runs, keep only
    # cluster centroids in internal RAM, and a query reads whole runs sequentially instead of
    # seeking per body. 4,096 centroid prefixes cost 0.4 ms to screen, and each 1 MB run costs
    # 58 ms to read plus 3.5 ms to compare. Four runs is 250 ms and fits the deep deadline, at the
    # cost of searching 0.1% of the card -- which coverage reports honestly.
    "teensy_sd": dict(
        count=9, capacity=3932160, us_per_vec=1485.9, stall_p=0.00,
        source="MEASURED: 4.03 GB FAT32 card, random 1 KB read 1485.9 us mean / 2800 us worst, "
               "sequential 17.1 MB/s, write 9.7 MB/s. Heatsink held 50 C at 600 MHz with no "
               "throttling across the whole run."),
    "fpga": dict(
        count=2, capacity=1048576, us_per_vec=0.33, stall_p=0.00,
        source="MEASURED area (yosys, 797 LCs at W=256); ESTIMATED bandwidth "
               "(32-bit PS DDR3, ~3 GB/s through AXI HP) = 3M vectors/s."),
}

# Which tiers answer first. The fast tier is asked on every query. The deep tier is only woken when
# the fast tier fails to recognise anything, which is the entire reason for having tiers.
FAST_TIER = ("teensy", "esp32s3", "luckfox")
DEEP_TIER = ("psram", "fpga", "luckfox_sd")

DEADLINE_FAST_US = 50_000        # 50 ms, the value hdc_node defaults to
DEADLINE_DEEP_US = 500_000       # 500 ms: the deep tier is allowed to be slow, not allowed to lie


# ---------------------------------------------------------------------------------------------
#  A node
# ---------------------------------------------------------------------------------------------
class Node:
    def __init__(self, nid, cls, cap, us_per_vec, stall_p, base, rng):
        self._rng = rng
        # Any node whose memory outruns its deadline screens on a prefix instead of scanning.
        # Measurement moved the Teensy into that group: 16,384 vectors at 31.34 us is 513 ms
        # against a 50 ms deadline. The two indexed classes are not alike, which is why the
        # geometry is per class rather than one constant:
        #
        #   Teensy       128-bit prefix, screened from INTERNAL RAM at 0.10 us an entry.
        #                512 bits would be 1.18 MB of index and the board has about 384 KB
        #                free, so the prefix has to be short. 128 bits over 16,384 entries
        #                still separates cleanly -- the refusal rate below is the check.
        #   PSRAM stick  512-bit prefix out of the Luckfox's DDR2 at 0.03 us an entry.
        #                Plenty of RAM there, so the prefix can be long and selective.
        cfg = SPEEDS[cls]
        self.indexed = "index_bits" in cfg
        self.prefix_bits = cfg.get("index_bits", 0)
        self.prefix_us = cfg.get("index_us", 0.0)
        self.index_mem_b = cfg.get("index_mem_b", 0)
        # 6 sigma over the prefix; sigma of a Hamming distance across P bits is sqrt(P)/2.
        self.prefix_gate = int(6 * (self.prefix_bits ** 0.5) / 2) if self.prefix_bits else 0
        self.nid = nid
        self.cls = cls
        self.cap = cap
        self.us = us_per_vec
        self.stall_p = stall_p
        self.base = base
        self.mem = []            # stored hypervectors
        self.proto = []          # concepts formed while consolidating
        self.proto_support = []
        self.support = []        # how many instances each stored vector now represents

    def reinforce(self, local, v):
        """Move a stored vector part way toward a new instance of the same thing.

        This is what makes the surprise gate compatible with abstraction. Discarding a recognised
        experience keeps memory small but means a node never accumulates the repetitions a
        prototype is made from, so consolidation can never happen -- which is exactly what the
        first run of this simulation reported. Reinforcing instead lets the STORED vector drift
        toward the centre of its concept, so the exemplar becomes the abstraction.

        The step shrinks as support grows, which is a running average: after n instances each new
        one moves it by 1/n. Early experience shapes a concept quickly, later experience refines it.
        """
        n = self.support[local] + 1
        self.support[local] = n
        diff = set_bits(self.mem[local] ^ v)
        if not diff:
            return
        k = max(1, len(diff) // n)
        keep = 0
        for b in self._rng.sample(diff, k):
            keep |= 1 << b
        self.mem[local] ^= keep

    def full(self):
        return len(self.mem) >= self.cap

    def scan(self, q, deadline_us, rng):
        """Return (best_local, best_dist, scanned, total, elapsed_us).

        Obeys hd_scan_chunk's rule exactly: strictly less-than while walking indices upward, which
        is lowest-index-wins on a tie. A node that runs out of deadline answers PARTIALLY -- it
        does not fail, and it does not pretend to have searched more than it did.
        """
        if rng.random() < self.stall_p:
            return HD_NO_SLOT, HD_FAR, 0, len(self.mem), 0.0

        if self.indexed:
            return self._scan_indexed(q, deadline_us)

        budget = int(deadline_us / self.us) if self.us > 0 else len(self.mem)
        n = min(budget, len(self.mem))

        best_d, best_i = HD_FAR, HD_NO_SLOT
        for i in range(n):
            d = popcount(self.mem[i] ^ q)
            if d < best_d:                        # STRICTLY less than
                best_d, best_i = d, i
        return best_i, best_d, n, len(self.mem), n * self.us

    def _scan_indexed(self, q, deadline_us):
        """The PSRAM tier, screened on a prefix instead of scanned.

        The first version of this simulation scanned the sticks linearly and reported 27 seconds for
        a full sweep of six of them. That is not a slow tier, it is an unusable one, and it is why
        bench_index.c exists: the 512-bit PREFIX of every entry lives in the Luckfox's own DDR2 and
        is compared at RAM speed, and only entries that survive that screen have their 1 KB body
        pulled over SPI. Screening 81,920 entries costs about 2.5 ms; the handful of bodies that
        follow cost 330 us each. Same capacity, three orders of magnitude less time.

        A query the index cannot separate from the crowd is REFUSED rather than answered, and the
        refusal reports zero coverage -- "I could not tell" rather than "it is not here".
        """
        mask = (1 << self.prefix_bits) - 1
        qp = q & mask
        pmin = self.prefix_bits + 1
        for v in self.mem:
            d = popcount((v & mask) ^ qp)
            if d < pmin:
                pmin = d
        gate = pmin + self.prefix_gate
        survivors = [i for i, v in enumerate(self.mem) if popcount((v & mask) ^ qp) <= gate]

        us = len(self.mem) * self.prefix_us * 2      # two screening passes, as bench_index does

        # The safety valve: too many survivors means the index cannot resolve this, and fetching
        # them all would be slow AND a coin flip. Refuse before reading a single body.
        if len(survivors) > 64:
            return HD_NO_SLOT, HD_FAR, 0, len(self.mem), us

        best_d, best_i = HD_FAR, HD_NO_SLOT
        for i in survivors:
            us += self.us                            # one body off the stick
            if us > deadline_us:
                break
            d = popcount(self.mem[i] ^ q)
            if d < best_d:
                best_d, best_i = d, i
        return best_i, best_d, len(self.mem), len(self.mem), us


# ---------------------------------------------------------------------------------------------
#  The coordinator's merge -- bench_hdc_shard.c's hd_merge_add, rule for rule
# ---------------------------------------------------------------------------------------------
class Merge:
    def __init__(self):
        self.best_dist = HD_FAR
        self.best_global = None
        self.best_node = None
        self.scanned = 0
        self.total = 0
        self.node_scanned = {}
        self.node_total = {}

    def add(self, node, best_local, best_dist, scanned, total):
        # Coverage keeps the BEST report per node and adjusts by the difference, so a duplicate or
        # a retry can neither inflate the count nor be ignored.
        prev = self.node_scanned.get(node.nid, 0)
        if scanned > prev:
            self.scanned += scanned - prev
            self.node_scanned[node.nid] = scanned
        prevt = self.node_total.get(node.nid, 0)
        if total > prevt:
            self.total += total - prevt
            self.node_total[node.nid] = total

        if best_local == HD_NO_SLOT:
            return
        g = node.base + best_local
        if (self.best_global is None or best_dist < self.best_dist
                or (best_dist == self.best_dist and g < self.best_global)):
            self.best_dist = best_dist
            self.best_global = g
            self.best_node = node

    def coverage(self):
        return (self.scanned / self.total) if self.total else 0.0


# ---------------------------------------------------------------------------------------------
#  The machine
# ---------------------------------------------------------------------------------------------
class Stack:
    def __init__(self, rng, scale):
        self.rng = rng
        self._rng = rng
        self.nodes = []
        base = 0
        nid = 0
        for cls, s in SPEEDS.items():
            # A class recorded in the speed table but not placed in a tier is measured data, not a
            # participant. teensy_sd is there so its numbers are on the record; it needs the
            # clustered sequential scan described above before it can answer a query at all.
            if cls not in FAST_TIER and cls not in DEEP_TIER:
                continue
            cap = max(1, int(s["capacity"] * scale))
            for _ in range(s["count"]):
                self.nodes.append(Node(nid, cls, cap, s["us_per_vec"], s["stall_p"],
                                       base, rng))
                base += cap
                nid += 1
        self.truth = {}              # global slot -> the label stored there

    def capacity(self, tiers=None):
        return sum(n.cap for n in self.nodes if tiers is None or n.cls in tiers)

    def stored(self):
        return sum(len(n.mem) for n in self.nodes)

    # --- learning ------------------------------------------------------------------------
    def learn(self, v, label):
        """Store an experience, but only if it is surprising -- otherwise sharpen what is there.

        Storing everything fills a node with a thousand copies of one thing and leaves no room for
        the thousand-and-first that is genuinely new. But DISCARDING a recognised experience is
        equally wrong, and the first run of this simulation proved it: with the gate simply throwing
        recognised input away, no node ever accumulated two instances of the same concept, so
        consolidation had nothing to work from and formed zero prototypes.

        Reinforcing is the resolution. A recognised experience moves the stored vector part way
        toward itself, so the exemplar drifts to the centre of its concept and BECOMES the
        abstraction. Memory stays small and learning still happens.
        """
        m = self.query(v, tiers=FAST_TIER, deadline_us=DEADLINE_FAST_US)
        if m.best_dist < RECOGNISE and self.truth.get(m.best_global) == label:
            node = m.best_node
            node.reinforce(m.best_global - node.base, v)
            return "reinforced"

        # Fill the fast tier first, then spill outward. A fast node with room is worth more than a
        # slow one, so the newest experience goes to the cheapest place that can hold it.
        for tiers in (FAST_TIER, DEEP_TIER):
            candidates = [n for n in self.nodes if n.cls in tiers and not n.full()]
            if candidates:
                n = min(candidates, key=lambda x: len(x.mem) / x.cap)
                self.truth[n.base + len(n.mem)] = label
                n.mem.append(v)
                n.support.append(1)
                return "stored"
        return "full"

    def sharpness(self, concepts, instance_of, samples=40):
        """Mean distance from a brand-new instance to whatever the machine thinks it is.

        This is the number that says whether learning is happening at all. It should FALL: as an
        exemplar drifts toward the centre of its concept, unseen instances of that concept land
        closer to it. Recall alone cannot show this, because recall saturates at 100% while the
        machine is still getting better.
        """
        tot = 0.0
        n = 0
        for _ in range(samples):
            c = self._rng.randrange(len(concepts))
            m = self.query(instance_of(c), FAST_TIER, DEADLINE_FAST_US)
            if m.best_dist != HD_FAR:
                tot += m.best_dist
                n += 1
        return (tot / n) if n else float("nan")

    def consolidate(self):
        """Dreaming: find exemplars that turned out to be the same thing and merge them.

        In a distributed memory two nodes can independently store the same concept, because neither
        could see the other's contents at the moment it decided the input was novel. Nothing is
        wrong at the time and nothing detects it later during a query -- both answer, and the merge
        picks whichever is nearer. What it costs is capacity, and sharpness: two half-trained
        exemplars are each worse than one fully-trained one.

        So while idle, look across nodes for stored vectors that now sit within recognition distance
        of each other, replace them with their majority, and free the duplicate. This is the only
        operation here that needs a global view, which is exactly why it happens when nothing else
        is going on.
        """
        # How full the fast tier is. Above this, capacity is worth more than the last few bits
        # of recognition accuracy and pairs get merged too.
        fast = [n for n in self.nodes if n.cls in FAST_TIER]
        occ = sum(len([v for v in n.mem if v]) for n in fast) / max(1, sum(n.cap for n in fast))
        pressure = occ > 0.80

        entries = []
        for n in self.nodes:
            for i in range(len(n.mem)):
                entries.append((n, i))

        merged = 0
        reclaimed = 0
        self.last_pressure = pressure
        dead = set()
        for a in range(len(entries)):
            if a in dead:
                continue
            na, ia = entries[a]
            group = [a]
            for b in range(a + 1, len(entries)):
                if b in dead:
                    continue
                nb, ib = entries[b]
                if self.truth.get(na.base + ia) != self.truth.get(nb.base + ib):
                    continue
                if popcount(na.mem[ia] ^ nb.mem[ib]) < RECOGNISE:
                    group.append(b)
            # MERGING A PAIR IS A LOSS, AND THE SIMULATION PROVED IT TWICE.
            #
            # Take two exemplars A and B of one concept, each carrying d wrong bits. They agree
            # almost everywhere and differ in about 2d places, and at each of those one is right
            # and the other is wrong. A majority of two has no majority, so the tie-break picks at
            # random and gets half of them wrong: the merged vector still carries about d errors.
            # No better than what went in -- and now there is ONE candidate where there were two,
            # so every query loses the best-of-two match it used to get. Recognition got worse both
            # times this ran unconditionally, 889 to 1043 and then 716 to 1038.
            #
            # Three or more members is a real majority and genuinely averages the noise away. Below
            # that, merging only buys capacity, so it is worth doing when capacity is scarce and a
            # waste when it is not. Dreaming is driven by pressure, not by the clock.
            if len(group) < 3 and not pressure:
                continue
            if len(group) < 2:
                continue

            # KEEP THE SURVIVOR ON THE FASTEST NODE IN THE GROUP.
            #
            # The obvious implementation keeps whichever member the loop reached first, and that
            # is wrong in a way the simulation caught immediately: recognition got WORSE after
            # consolidating, 716 to 1039. Merging a fast-tier exemplar into a deep-tier one
            # DEMOTES the concept. The machine still knows it, but now it only finds out after
            # waking the slow tier, so every query for a concept it has learned well pays deep-tier
            # latency. Consolidation must never move something into slower storage.
            group.sort(key=lambda g: (entries[g][0].cls not in FAST_TIER,
                                      entries[g][0].us))
            keep = group[0]
            na, ia = entries[keep]

            members = [entries[g][0].mem[entries[g][1]] for g in group]
            na.mem[ia] = majority(members, self._rng)
            na.support[ia] = sum(entries[g][0].support[entries[g][1]] for g in group)
            for g in group:
                if g == keep:
                    continue
                dead.add(g)
                ng, ig = entries[g]
                ng.mem[ig] = 0                      # freed: distance to anything is ~4096, so it
                self.truth.pop(ng.base + ig, None)  # never wins a comparison again
                reclaimed += 1
            dead.add(keep)                          # already merged; do not re-group it
            merged += 1
        return merged, reclaimed

    # --- answering -----------------------------------------------------------------------
    def query(self, q, tiers, deadline_us):
        m = Merge()
        slowest = 0.0
        for n in self.nodes:
            if n.cls not in tiers or not n.mem:
                continue
            bl, bd, sc, tot, us = n.scan(q, deadline_us, self.rng)
            m.add(n, bl, bd, sc, tot)
            slowest = max(slowest, us)
        m.latency_us = slowest        # nodes answer in parallel, so the slowest sets the latency
        return m

    def ask(self, q):
        """The intended two-tier behaviour: fast first, deep only when the fast tier is unsure."""
        fast = self.query(q, FAST_TIER, DEADLINE_FAST_US)
        if fast.best_dist < RECOGNISE:
            return fast, "fast", fast.latency_us
        deep = self.query(q, DEEP_TIER, DEADLINE_DEEP_US)
        # Merge the two tiers' answers through the same algebra. This is the monoid doing real work:
        # two independent searches combine with no special case.
        combined = Merge()
        for src in (fast, deep):
            if src.best_node is not None:
                combined.add(src.best_node, src.best_global - src.best_node.base,
                             src.best_dist, src.node_scanned.get(src.best_node.nid, 0),
                             src.node_total.get(src.best_node.nid, 0))
        combined.scanned = fast.scanned + deep.scanned
        combined.total = fast.total + deep.total
        return combined, "deep", fast.latency_us + deep.latency_us


# ---------------------------------------------------------------------------------------------
def main():
    n_vectors, n_queries, scale = 3000, 250, 0.02
    for i, a in enumerate(sys.argv):
        if a == "--vectors" and i + 1 < len(sys.argv):
            n_vectors = int(sys.argv[i + 1])
        if a == "--queries" and i + 1 < len(sys.argv):
            n_queries = int(sys.argv[i + 1])
        if a == "--scale" and i + 1 < len(sys.argv):
            scale = float(sys.argv[i + 1])

    rng = random.Random(0x8E4C11)
    stack = Stack(rng, scale)

    print()
    print("=" * 71)
    print("BENCH ONE  --  whole stack at the speeds the real parts deliver")
    print("=" * 71)
    print()
    print("  class     n   capacity      us/vector   stalls   full-tier scan")
    print("  -------  --  ----------  -----------  -------  ---------------")
    for cls, s in SPEEDS.items():
        full = s["count"] * s["capacity"] * s["us_per_vec"] / 1e6 / s["count"]
        print("  %-7s %3d  %10d  %11.2f  %6.0f%%  %12.3f s"
              % (cls, s["count"], s["count"] * s["capacity"], s["us_per_vec"],
                 s["stall_p"] * 100, full))
    print()
    print("  total real capacity: %s vectors (%.1f GB at %d B each)"
          % (f"{sum(s['count'] * s['capacity'] for s in SPEEDS.values()):,}",
             sum(s["count"] * s["capacity"] for s in SPEEDS.values()) * HD_BYTES / 1e9,
             HD_BYTES))
    print("  simulated at scale %.3f: %s slots across %d nodes"
          % (scale, f"{stack.capacity():,}", len(stack.nodes)))
    print()
    for cls, s in SPEEDS.items():
        print("  %-8s %s" % (cls, s["source"]))
    print()

    # --- build a world of concepts, each with several instances -------------------------
    n_concepts = max(4, n_vectors // 12)
    concepts = [rng.getrandbits(HD_BITS) for _ in range(n_concepts)]

    def instance_of(c, noise=400):
        v = concepts[c]
        for _ in range(noise):
            v ^= 1 << rng.randrange(HD_BITS)
        return v

    print("=" * 71)
    print("[1] LEARNING -- %d experiences of %d concepts, surprise-gated" % (n_vectors, n_concepts))
    stored = reinforced = full = 0
    sharp_early = None
    for i in range(n_vectors):
        c = rng.randrange(n_concepts)
        r = stack.learn(instance_of(c), c)
        stored += r == "stored"
        reinforced += r == "reinforced"
        full += r == "full"
        if i == n_vectors // 5:
            sharp_early = stack.sharpness(concepts, instance_of)
    sharp_late = stack.sharpness(concepts, instance_of)
    print("    stored %d, reinforced an existing exemplar %d, refused for lack of room %d"
          % (stored, reinforced, full))
    print("    distance from a NEW instance to its match: %.0f early, %.0f after learning"
          % (sharp_early, sharp_late))
    print("    (random pairs sit at %d, so lower is sharper; recall saturates but this keeps moving)"
          % RANDOM_MEAN)
    print("    occupancy %d of %d slots (%.0f%%)"
          % (stack.stored(), stack.capacity(), 100.0 * stack.stored() / stack.capacity()))
    if reinforced:
        print("    the gate did its job: %.0f%% of experiences needed no new storage"
              % (100.0 * reinforced / n_vectors))

    print()
    print("[2] CONSOLIDATION -- merging exemplars two nodes stored independently")
    merged, reclaimed = stack.consolidate()
    sharp_after = stack.sharpness(concepts, instance_of)
    print("    %d groups merged, %d slots reclaimed" % (merged, reclaimed))
    print("    distance to a new instance after merging: %.0f (was %.0f)"
          % (sharp_after, sharp_late))

    print()
    print("[3] ANSWERING -- %d queries, fast tier first" % n_queries)
    hit = miss = wrong_confident = 0
    by_tier = {"fast": 0, "deep": 0}
    lat = []
    over_deadline = 0
    cov_lies = 0

    for _ in range(n_queries):
        c = rng.randrange(n_concepts)
        q = instance_of(c)
        m, tier, latency = stack.ask(q)
        by_tier[tier] += 1
        lat.append(latency)
        if latency > DEADLINE_FAST_US + DEADLINE_DEEP_US:
            over_deadline += 1
        if m.coverage() > 1.0001:
            cov_lies += 1

        got = stack.truth.get(m.best_global)
        if m.best_dist < RECOGNISE:
            if got == c:
                hit += 1
            else:
                wrong_confident += 1
        else:
            miss += 1

    lat.sort()
    print("    answered from the fast tier %d, needed the deep tier %d"
          % (by_tier["fast"], by_tier["deep"]))
    print("    correct %d, said 'I do not recognise this' %d, CONFIDENTLY WRONG %d"
          % (hit, miss, wrong_confident))
    print("    latency  median %.1f ms   p95 %.1f ms   worst %.1f ms"
          % (lat[len(lat) // 2] / 1000.0, lat[int(len(lat) * 0.95)] / 1000.0, lat[-1] / 1000.0))

    print()
    print("  at FULL hardware capacity, worst case per node -- the real deadline test:")
    fits_all = True
    for cls in FAST_TIER + DEEP_TIER:
        sp = SPEEDS[cls]
        cap = sp["capacity"]
        if "index_bits" in sp:
            screen = cap * sp["index_us"] * 2 / 1000.0            # two passes, as bench_index does
            bodies = 64 * sp["us_per_vec"] / 1000.0               # the survivor cap
            t = screen + bodies
            note = ("%d-bit prefix, %.2f ms screening + %.2f ms for 64 bodies, index %d KB"
                    % (sp["index_bits"], screen, bodies, cap * sp["index_mem_b"] // 1024))
            linear = cap * sp["us_per_vec"] / 1000.0
        else:
            t = cap * sp["us_per_vec"] / 1000.0
            note = "scanned linearly"
            linear = t
        budget = DEADLINE_FAST_US / 1000.0 if cls in FAST_TIER else DEADLINE_DEEP_US / 1000.0
        ok = t <= budget
        fits_all = fits_all and ok
        print("    %-9s %8.2f ms  %-6s  %s" % (cls, t, "fits" if ok else "OVER", note))
        if "index_bits" in sp:
            print("              %8.2f ms if it were scanned linearly instead -- %.0fx worse"
                  % (linear, linear / t))
    print()
    print()
    print("=" * 71)
    print("DOES IT OPERATE AS INTENDED?")
    print("=" * 71)
    checks = [
        ("no confidently wrong answers", wrong_confident == 0,
         "%d queries were answered confidently and wrongly" % wrong_confident),
        ("no query exceeded its deadline", over_deadline == 0,
         "%d queries ran past the deadline budget" % over_deadline),
        ("coverage never exceeds 100%", cov_lies == 0,
         "%d queries reported more searched than exists" % cov_lies),
        ("the fast tier carries the load", by_tier["fast"] > by_tier["deep"],
         "the deep tier answered more often than the fast tier"),
        ("recall above 90%", hit >= 0.90 * n_queries,
         "recall was %.0f%%" % (100.0 * hit / n_queries)),
        ("the surprise gate suppressed repeats", reinforced > 0,
         "every experience was stored, so the gate is not working"),
        ("learning sharpened the exemplars", sharp_late < sharp_early,
         "distance to new instances did not fall: %.0f then %.0f" % (sharp_early, sharp_late)),
        ("every node fits its deadline at FULL capacity", fits_all,
         "a node cannot cover its own memory inside its deadline"),
        ("consolidation did not blunt them", sharp_after <= sharp_late + SIGMA,
         "merging made recognition worse: %.0f then %.0f" % (sharp_late, sharp_after)),
    ]
    bad = 0
    for name, ok, why in checks:
        print("  %-34s %s" % (name, "yes" if ok else "NO   -- " + why))
        if not ok:
            bad += 1

    print()
    print("=" * 71)
    print("OPERATES AS INTENDED" if not bad else "%d PROPERTY(IES) VIOLATED" % bad)
    print("=" * 71)
    print()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
