#!/usr/bin/env python3
"""
tmds_ref.py -- DVI 1.0 TMDS reference model (encoder + decoder) for the scanout tests.

Commands:
  tmds_ref.py gen   <n_symbols> <seed> <vec_file>   write stimulus + expected symbols
  tmds_ref.py check <vec_file> <dump_file>          compare RTL dump, decode, disparity checks
  tmds_ref.py bound                                 print the reachable running-disparity set
  tmds_ref.py selftest                              internal consistency checks

Vector file (one line per clock, hex, $readmemh-compatible, 24 bits):
  [20]    de
  [19:18] {c1, c0}
  [17:10] d[7:0]
  [9:0]   expected TMDS symbol (q_out[9:0]; bit 0 is transmitted first)
Dump file (written by tb_tmds.v): one 3-digit hex symbol per line, same order as the vectors.
"""
import random
import sys

# Control tokens, keyed by {c1, c0}.  q_out[9:0] written MSB first (bit 0 is sent first).
CTRL = {0: 0b1101010100, 1: 0b0010101011, 2: 0b0101010100, 3: 0b1010101011}
CTRL_INV = {v: k for k, v in CTRL.items()}


def popcount(x):
    return bin(x).count("1")


def tmds_encode(cnt, de, d, c):
    """One DVI 1.0 encoder step.  Returns (symbol, new_cnt, branch)."""
    if not de:
        return CTRL[c & 3], 0, "ctrl"
    n1d = popcount(d)
    use_xnor = n1d > 4 or (n1d == 4 and (d & 1) == 0)
    qm = d & 1
    for i in range(1, 8):
        prev = (qm >> (i - 1)) & 1
        di = (d >> i) & 1
        b = (prev ^ di) ^ 1 if use_xnor else (prev ^ di)
        qm |= b << i
    qm8 = 0 if use_xnor else 1
    n1 = popcount(qm)
    n0 = 8 - n1
    if cnt == 0 or n1 == n0:
        q9 = qm8 ^ 1
        q70 = qm if qm8 else (~qm) & 0xFF
        sym = (q9 << 9) | (qm8 << 8) | q70
        cnt = cnt + (n0 - n1) if qm8 == 0 else cnt + (n1 - n0)
        br = "bal"
    elif (cnt > 0 and n1 > n0) or (cnt < 0 and n0 > n1):
        sym = (1 << 9) | (qm8 << 8) | ((~qm) & 0xFF)
        cnt = cnt + 2 * qm8 + (n0 - n1)
        br = "inv"
    else:
        sym = (0 << 9) | (qm8 << 8) | qm
        cnt = cnt - 2 * (1 - qm8) + (n1 - n0)
        br = "noinv"
    return sym, cnt, br + ("_xnor" if use_xnor else "_xor")


def tmds_decode(sym):
    """Returns ('C', c1c0) for a control token, ('D', byte) otherwise."""
    if sym in CTRL_INV:
        return "C", CTRL_INV[sym]
    d = sym & 0xFF
    if (sym >> 9) & 1:
        d = (~d) & 0xFF
    out = d & 1
    xor_mode = (sym >> 8) & 1
    for i in range(1, 8):
        x = ((d >> i) & 1) ^ ((d >> (i - 1)) & 1)
        if not xor_mode:
            x ^= 1
        out |= x << i
    return "D", out


def sym_disparity(sym):
    n1 = popcount(sym & 0x3FF)
    return n1 - (10 - n1)


def reachable_cnt():
    seen = {0}
    todo = [0]
    while todo:
        c = todo.pop()
        for d in range(256):
            _, n, _ = tmds_encode(c, 1, d, 0)
            if n not in seen:
                seen.add(n)
                todo.append(n)
    return sorted(seen)


def gen(n, seed, path):
    rng = random.Random(seed)
    extremes = [0x00, 0xFF, 0x01, 0xFE, 0x80, 0x7F, 0x0F, 0xF0, 0x55, 0xAA, 0x10, 0xEF]
    stim = []
    while len(stim) < n:
        kind = rng.random()
        if kind < 0.40:                       # uniform random data run
            for _ in range(rng.randint(1, 3000)):
                stim.append((1, 0, rng.randrange(256)))
        elif kind < 0.60:                     # biased data run (drives the disparity hard)
            pool = rng.sample(extremes, rng.randint(1, 3))
            for _ in range(rng.randint(1, 500)):
                stim.append((1, 0, rng.choice(pool)))
        elif kind < 0.70:                     # exhaustive sweep of all byte values
            order = list(range(256))
            rng.shuffle(order)
            for d in order:
                stim.append((1, 0, d))
        elif kind < 0.90:                     # control period
            c = rng.randrange(4)
            for _ in range(rng.randint(1, 200)):
                if rng.random() < 0.1:
                    c = rng.randrange(4)
                stim.append((0, c, rng.randrange(256)))   # data ignored in blanking
        else:                                 # rapid de toggling
            for _ in range(rng.randint(2, 100)):
                de = rng.randrange(2)
                stim.append((de, rng.randrange(4), rng.randrange(256)))
    stim = stim[:n]
    cnt = 0
    cov = {}
    cnts = set()
    with open(path, "w") as f:
        for de, c, d in stim:
            sym, cnt, br = tmds_encode(cnt, de, d, c)
            cov[br] = cov.get(br, 0) + 1
            cnts.add(cnt)
            f.write("%06x\n" % ((de << 20) | (c << 18) | (d << 10) | sym))
    print("gen: %d vectors, seed %d -> %s" % (len(stim), seed, path))
    print("gen: branch coverage:", ", ".join("%s=%d" % kv for kv in sorted(cov.items())))
    print("gen: cnt values reached:", sorted(cnts))
    return 0


def check(vec_path, dump_path):
    vecs = [int(l, 16) for l in open(vec_path) if l.strip()]
    dump = [l.strip() for l in open(dump_path) if l.strip()]
    if len(dump) != len(vecs):
        print("check: FAIL: %d vectors but %d dumped symbols" % (len(vecs), len(dump)))
        return 1
    bound = max(abs(c) for c in reachable_cnt())
    errs = 0
    cnt = 0          # re-encoded reference state
    disp = 0         # disparity measured on the RTL symbol stream itself
    maxdisp = 0
    ndata = nctrl = 0
    for i, (v, s) in enumerate(zip(vecs, dump)):
        if "x" in s.lower() or "z" in s.lower():
            print("check: FAIL at %d: RTL symbol has X/Z (%s)" % (i, s))
            errs += 1
            if errs > 10:
                break
            continue
        rtl = int(s, 16)
        de, c, d, exp = (v >> 20) & 1, (v >> 18) & 3, (v >> 10) & 0xFF, v & 0x3FF
        ref, cnt, _ = tmds_encode(cnt, de, d, c)
        if ref != exp:
            print("check: FAIL at %d: vector file inconsistent" % i)
            return 1
        if rtl != exp:
            errs += 1
            if errs <= 10:
                print("check: FAIL at %d: de=%d c=%d d=%02x rtl=%03x exp=%03x" % (i, de, c, d, rtl, exp))
            continue
        kind, val = tmds_decode(rtl)
        if de:
            ndata += 1
            if kind != "D" or val != d:
                errs += 1
                if errs <= 10:
                    print("check: FAIL at %d: decode of %03x = %s %02x, sent %02x" % (i, rtl, kind, val, d))
            disp += sym_disparity(rtl)
            if disp != cnt:
                errs += 1
                if errs <= 10:
                    print("check: FAIL at %d: stream disparity %d != cnt %d" % (i, disp, cnt))
            maxdisp = max(maxdisp, abs(disp))
            if abs(disp) > bound:
                errs += 1
                if errs <= 10:
                    print("check: FAIL at %d: |disparity| %d > bound %d" % (i, disp, bound))
        else:
            nctrl += 1
            disp = 0
            if kind != "C" or val != c:
                errs += 1
                if errs <= 10:
                    print("check: FAIL at %d: control decode %s %d, sent %d" % (i, kind, val, c))
    print("check: %d symbols (%d data, %d control), max |running disparity| = %d (bound %d), errors = %d"
          % (len(vecs), ndata, nctrl, maxdisp, bound, errs))
    print("check: %s" % ("PASS" if errs == 0 else "FAIL"))
    return 0 if errs == 0 else 1


def selftest():
    errs = 0
    # every byte value from every reachable disparity state round-trips; tokens are distinct
    # from all data symbols; the data symbol set has 460 members.
    datasyms = set()
    for c in reachable_cnt():
        for d in range(256):
            s, _, _ = tmds_encode(c, 1, d, 0)
            datasyms.add(s)
            k, v = tmds_decode(s)
            if k != "D" or v != d:
                errs += 1
    for t in CTRL.values():
        if t in datasyms:
            errs += 1
    print("selftest: %d distinct data symbols (expect 460), reachable cnt %s, errors %d"
          % (len(datasyms), reachable_cnt(), errs))
    return 0 if errs == 0 and len(datasyms) == 460 else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "gen" and len(a) == 4:
        sys.exit(gen(int(a[1]), int(a[2]), a[3]))
    if a and a[0] == "check" and len(a) == 3:
        sys.exit(check(a[1], a[2]))
    if a and a[0] == "bound":
        print(reachable_cnt())
        sys.exit(0)
    if a and a[0] == "selftest":
        sys.exit(selftest())
    print(__doc__)
    sys.exit(2)
