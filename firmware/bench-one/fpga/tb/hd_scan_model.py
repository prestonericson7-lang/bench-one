#!/usr/bin/env python3
"""
hd_scan_model.py -- cross-check hd_scan.v's design against bench_hdc_shard.c

WHAT THIS DOES AND DOES NOT PROVE
---------------------------------
There is no Verilog simulator and no Vivado on this machine, so hd_scan.v has not been
elaborated. This is not that. This is a cycle-accurate model of hd_scan.v's state machine,
written to the same non-blocking-assignment semantics, checked against an independent
implementation of the C's rules.

It therefore catches the class of bug that would actually ruin the FPGA link: the commit landing
one cycle off the pipeline register, the beat counter wrapping wrong, a halt mid-vector leaking a
partial distance or a phantom count, a tie going to the wrong index. Those are silent. They
produce a system that works on the bench and disagrees with itself once a week.

It does not catch Verilog syntax or elaboration errors. Vivado reports those the first time the
project is opened, loudly, which is the difference.

WHY THE MODEL IS WRITTEN THIS WAY
---------------------------------
Every signal is read from the current state and written into a separate next state, and later
writes in the same cycle overwrite earlier ones. That is exactly what a Verilog always block with
non-blocking assignments does, and it is the only way the model can be evidence about the RTL
rather than a second guess at the same idea. The assignment order below follows hd_scan.v top to
bottom on purpose.

RUN
    python hd_scan_model.py
    python hd_scan_model.py --trials 5000
"""

import random
import sys

HD_BITS = 8192
W = 512
BEATS = HD_BITS // W          # 16
HD_NO_SLOT = 0xFFFF
HD_FAR = 0xFFFFFFFF

S_IDLE, S_RUN, S_DRAIN, S_DONE = 0, 1, 2, 3


def popcount(x):
    return bin(x).count("1")


class HdScan:
    """A cycle-accurate model of hd_scan.v."""

    def __init__(self):
        self.s = dict(
            state=S_IDLE, busy=0, done=0,
            beat=0, vec_done=0, acc=0,
            x_r=0, x_vld=0, x_last=0,
            best_dist=HD_FAR, best_local=HD_NO_SLOT,
            r_node=0, r_qid=0, r_base=0, r_total=0,
            o_best_local=HD_NO_SLOT, o_best_dist=HD_FAR, o_scanned=0,
        )
        self.qmem = [0] * BEATS

    # --- combinational -----------------------------------------------------------------
    def s_ready(self, halt):
        s = self.s
        return (s["state"] == S_RUN) and (not halt) and (s["vec_done"] != s["r_total"])

    # --- one rising edge ---------------------------------------------------------------
    def clk(self, start=0, halt=0, s_valid=0, s_data=0, cfg=None, q_wr=0, q_addr=0, q_data=0):
        s = self.s
        n = dict(s)                                  # next state starts as current

        if q_wr:
            self.qmem[q_addr] = q_data

        ready = self.s_ready(halt)
        accept = bool(s_valid and ready)

        pc = popcount(s["x_r"])
        acc_next = s["acc"] + pc

        # done <= 0
        n["done"] = 0

        # stage 0: XOR the incoming beat against the query
        if accept:
            n["x_r"] = s_data ^ self.qmem[s["beat"]]
            n["x_vld"] = 1
            n["x_last"] = 1 if s["beat"] == BEATS - 1 else 0
            n["beat"] = 0 if s["beat"] == BEATS - 1 else s["beat"] + 1
        else:
            n["x_vld"] = 0

        # stage 1: popcount, accumulate, commit on the last beat
        if s["x_vld"]:
            if s["x_last"]:
                if acc_next < s["best_dist"]:         # STRICTLY less than
                    n["best_dist"] = acc_next
                    n["best_local"] = s["vec_done"]
                n["vec_done"] = s["vec_done"] + 1
                n["acc"] = 0
            else:
                n["acc"] = acc_next

        # control -- these writes come later in the always block, so they win
        st = s["state"]
        if st == S_IDLE:
            if start:
                c = cfg or {}
                n["r_node"] = c.get("node", 0)
                n["r_qid"] = c.get("query_id", 0)
                n["r_base"] = c.get("base", 0)
                n["r_total"] = c.get("total", 0)
                n["beat"] = 0
                n["vec_done"] = 0
                n["acc"] = 0
                n["x_vld"] = 0
                n["best_dist"] = HD_FAR
                n["best_local"] = HD_NO_SLOT
                n["busy"] = 1
                n["state"] = S_RUN
        elif st == S_RUN:
            if halt or s["vec_done"] == s["r_total"]:
                n["state"] = S_DRAIN
        elif st == S_DRAIN:
            if not s["x_vld"]:
                n["acc"] = 0
                n["beat"] = 0
                n["state"] = S_DONE
        elif st == S_DONE:
            n["o_best_local"] = s["best_local"]
            n["o_best_dist"] = s["best_dist"]
            n["o_scanned"] = s["vec_done"]
            n["busy"] = 0
            n["done"] = 1
            n["state"] = S_IDLE

        self.s = n
        return ready

    def wire(self):
        """hd_partial_pack's 16 bytes, little-endian, as the RTL's o_wire produces them."""
        s = self.s
        b = bytearray(16)
        b[0:2] = s["r_node"].to_bytes(2, "little")
        b[2:4] = s["r_qid"].to_bytes(2, "little")
        b[4:6] = s["r_base"].to_bytes(2, "little")
        b[6:8] = s["o_best_local"].to_bytes(2, "little")
        b[8:12] = s["o_best_dist"].to_bytes(4, "little")
        b[12:14] = s["o_scanned"].to_bytes(2, "little")
        b[14:16] = s["r_total"].to_bytes(2, "little")
        return bytes(b)


def reference(vectors, query, completed):
    """bench_hdc_shard.c's hd_scan_chunk, over the first `completed` vectors.

    Strict less-than while walking indices upward, which is lowest-index-wins on a tie.
    """
    best_dist, best_local = HD_FAR, HD_NO_SLOT
    for i in range(completed):
        d = sum(popcount(vectors[i][k] ^ query[k]) for k in range(BEATS))
        if d < best_dist:
            best_dist, best_local = d, i
    return best_local, best_dist


def run_trial(rng, force_ties=False):
    total = rng.randint(1, 24)
    query = [rng.getrandbits(W) for _ in range(BEATS)]

    vectors = []
    for i in range(total):
        if force_ties and i > 0 and rng.random() < 0.5:
            # Same distance as vector 0, different content. This is the case that separates
            # strict less-than from less-or-equal, and it is the one that breaks the merge.
            vectors.append(list(vectors[0]))
        else:
            vectors.append([rng.getrandbits(W) for _ in range(BEATS)])

    dut = HdScan()
    for a in range(BEATS):
        dut.clk(q_wr=1, q_addr=a, q_data=query[a])

    cfg = dict(node=rng.randint(0, 63), query_id=rng.randint(0, 0xFFFF),
               base=rng.randint(0, 1000), total=total)
    dut.clk(start=1, cfg=cfg)

    # Decide when to cut it short. Sometimes never, so the self-termination path is exercised too.
    halt_at = None
    if rng.random() < 0.7:
        halt_at = rng.randint(0, total * BEATS + 4)

    beats = [v[k] for v in vectors for k in range(BEATS)]

    # OVERRUN ON PURPOSE. A DMA feeding this core does not necessarily stop on the exact beat the
    # shard ends. If the core keeps accepting, those beats accumulate and commit as a candidate at
    # index r_total -- a confident answer pointing outside the shard, which the coordinator turns
    # into a global index belonging to some other node. Without these extra beats the guard that
    # prevents it is never exercised, and a mutation test proved exactly that.
    overrun = rng.randint(0, 3) * BEATS + rng.randint(0, BEATS)
    beats += [rng.getrandbits(W) for _ in range(overrun)]

    fed = 0
    accepted = 0
    cycles = 0

    while not dut.s["done"] and cycles < total * BEATS * 6 + 200:
        halt = 1 if (halt_at is not None and accepted >= halt_at) else 0
        # Gaps in s_valid on purpose: the host will not always have a beat ready.
        want = (fed < len(beats)) and (rng.random() < 0.85)
        data = beats[fed] if want else 0
        ready = dut.clk(halt=halt, s_valid=1 if want else 0, s_data=data)
        if want and ready:
            fed += 1
            accepted += 1
        cycles += 1

    if not dut.s["done"]:
        return "never finished"

    # Independent ground truth: whole vectors are however many complete groups of BEATS were
    # actually taken in. Derived from the feed, not from the model's own counter.
    completed = accepted // BEATS
    if completed > total:
        return ("accepted %d beats, %d past the end of a %d-vector shard"
                % (accepted, accepted - total * BEATS, total))
    exp_local, exp_dist = reference(vectors, query, completed)

    got = dut.s
    if got["o_scanned"] != completed:
        return "scanned %d, expected %d (accepted %d beats)" % (
            got["o_scanned"], completed, accepted)
    if got["o_best_local"] != exp_local:
        return "best_local %d, expected %d (scanned %d)" % (
            got["o_best_local"], exp_local, completed)
    if got["o_best_dist"] != exp_dist:
        return "best_dist %d, expected %d" % (got["o_best_dist"], exp_dist)

    w = dut.wire()
    if len(w) != 16:
        return "wire is %d bytes" % len(w)
    if int.from_bytes(w[0:2], "little") != cfg["node"]:
        return "wire node field wrong"
    if int.from_bytes(w[8:12], "little") != exp_dist:
        return "wire best_dist field wrong"
    return None


def main():
    trials = 2000
    for i, a in enumerate(sys.argv):
        if a == "--trials" and i + 1 < len(sys.argv):
            trials = int(sys.argv[i + 1])

    print()
    print("=" * 63)
    print("hd_scan.v design cross-check against bench_hdc_shard.c")
    print("  %d-bit vectors, %d bits per beat, %d beats each" % (HD_BITS, W, BEATS))
    print("=" * 63)
    print()

    rng = random.Random(0xBE0E)
    fails = 0

    for phase, ties in (("random vectors", False), ("deliberate distance ties", True)):
        bad = 0
        for t in range(trials):
            err = run_trial(rng, force_ties=ties)
            if err:
                if bad < 3:
                    print("  FAIL  %s: trial %d: %s" % (phase, t, err))
                bad += 1
        status = "%d failed" % bad if bad else "all passed"
        print("  %-26s %5d trials   %s" % (phase, trials, status))
        fails += bad

    print()
    print("  checked, on every trial:")
    print("    - best_local and best_dist match a full-precision reference scan")
    print("    - scanned counts only WHOLE vectors, derived from the feed independently")
    print("    - a halt mid-vector contributes neither a distance nor a count")
    print("    - gaps in s_valid change nothing")
    print("    - the 16-byte packet is little-endian in hd_partial_pack's field order")
    print()
    print("=" * 63)
    print("DESIGN LOGIC VERIFIED" if not fails else "%d FAILURES" % fails)
    print("Not verified here: Verilog elaboration. Vivado reports that on first open.")
    print("=" * 63)
    print()
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
