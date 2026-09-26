#!/usr/bin/env python3
# review/experiments/tmds_exhaustive.py -- independent DVI 1.0 TMDS reference (written for the
# round-1 review, NOT derived from sim/tmds_ref.py) + exhaustive state x input vector generator.
#
#   python3 tmds_exhaustive.py gen  <vec.txt>        write vectors: "rst de c1 c0 d expected_q"
#   python3 tmds_exhaustive.py check <vec.txt> <dut.txt>
#
# The reference follows DVI 1.0 section 3.2.3 figure 3-5 literally with unbounded Python ints.
# Coverage: BFS over the reachable running-disparity states; for every reachable cnt and every
# 8-bit input the vector file contains (reset, shortest path to cnt, the input) so the RTL is
# checked on all (state, input) pairs, plus all four control tokens from every state.
import sys
from collections import deque

CTRL = {0: 0b1101010100, 1: 0b0010101011, 2: 0b0101010100, 3: 0b1010101011}  # q[9:0], {c1,c0}

def n1(x, bits=8):
    return bin(x & ((1 << bits) - 1)).count("1")

def enc(cnt, de, d, c):
    """returns (q[9:0] as int, new cnt). cnt is a Python int (unbounded)."""
    if not de:
        return CTRL[c], 0
    # stage 1
    xnor = n1(d) > 4 or (n1(d) == 4 and (d & 1) == 0)
    qm = [d & 1]
    for i in range(1, 8):
        b = (d >> i) & 1
        v = qm[i - 1] ^ b
        qm.append(1 - v if xnor else v)
    qm8 = 0 if xnor else 1
    qm_val = sum(bit << i for i, bit in enumerate(qm))
    N1 = n1(qm_val)
    N0 = 8 - N1
    inv = lambda v: (~v) & 0xFF
    if cnt == 0 or N1 == N0:
        q9 = 1 - qm8
        q70 = qm_val if qm8 else inv(qm_val)
        cnt = cnt + (N1 - N0) if qm8 else cnt + (N0 - N1)
    elif (cnt > 0 and N1 > N0) or (cnt < 0 and N0 > N1):
        q9 = 1
        q70 = inv(qm_val)
        cnt = cnt + 2 * qm8 + (N0 - N1)
    else:
        q9 = 0
        q70 = qm_val
        cnt = cnt - 2 * (1 - qm8) + (N1 - N0)
    return (q9 << 9) | (qm8 << 8) | q70, cnt

def reachable():
    seen = {0: []}
    dq = deque([0])
    while dq:
        c = dq.popleft()
        for d in range(256):
            _, c2 = enc(c, 1, d, 0)
            if c2 not in seen:
                seen[c2] = seen[c] + [d]
                dq.append(c2)
    return seen

def gen(path):
    st = reachable()
    print("reachable running-disparity states:", sorted(st))
    lines = []
    ntests = 0
    for c, pth in sorted(st.items()):
        for x in list(range(256)) + [-1, -2, -3, -4]:
            lines.append("1 0 0 0 00 xxx")          # reset (cnt = 0)
            cnt = 0
            for d in pth:
                q, cnt = enc(cnt, 1, d, 0)
                lines.append("0 1 0 0 %02x %03x" % (d, q))
            assert cnt == c
            if x >= 0:
                q, _ = enc(cnt, 1, x, 0)
                lines.append("0 1 0 0 %02x %03x" % (x, q))
            else:
                cc = -x - 1
                q, _ = enc(cnt, 0, 0, cc)
                lines.append("0 0 %d %d 00 %03x" % (cc >> 1, cc & 1, q))
            # pad (not checked): the next test's reset clears q one clock after it is applied,
            # which would otherwise overwrite this test's symbol before it is sampled
            lines.append("0 0 0 0 00 xxx")
            ntests += 1
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("tests:", ntests, "vectors:", len(lines))

def check(vec, dut):
    exp = [l.split()[5] for l in open(vec) if l.strip()]
    got = [l.strip() for l in open(dut) if l.strip()]
    assert len(exp) == len(got), (len(exp), len(got))
    bad = 0
    for i, (e, g) in enumerate(zip(exp, got)):
        if e == "xxx":
            continue
        if int(g, 16) != int(e, 16):
            bad += 1
            if bad <= 10:
                print("MISMATCH vector %d: expected %s got %s" % (i, e, g))
    n = sum(1 for e in exp if e != "xxx")
    print("check: %d symbols compared, %d mismatches -> %s" % (n, bad, "PASS" if bad == 0 else "FAIL"))
    return bad == 0

if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(sys.argv[2])
    else:
        sys.exit(0 if check(sys.argv[2], sys.argv[3]) else 1)
