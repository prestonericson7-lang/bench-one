#!/usr/bin/env python3
"""
canlog.py -- decode logs from car-can-logger (CANLOG2 format).

This is the other half of the reverse-engineering rig. The Teensy captures every
frame with hardware-listen-only silence; this turns those bytes into named signals.

BMW's PHEV frames are not publicly documented, so the workflow is:
    1.  log a drive / a charge / a cold start
    2.  `candidates` -- let the tool surface bytes that BEHAVE like sensor values
    3.  `extract`    -- pull one to CSV and correlate against what the car was doing
    4.  write it down in signals.md

Stdlib only, so it runs on the Pi, the Luckfox, or here.

FORMAT (little-endian, append-only):
    header : "CANLOG2\\n"  + u32 bus1_baud + u32 bus2_baud          (16 bytes)
    record : u32 t_us | u8 bus | u8 lenflags | u32 id | data[len]
             lenflags: bits0-3 = dlc, bit4 = extended id, bit5 = remote
"""
import argparse
import struct
import sys
from collections import defaultdict

MAGIC = b"CANLOG2\n"
HDR = 16
REC = 10  # fixed part


def read_log(path):
    """Yield (t_us, bus, id, ext, remote, data). Tolerates a truncated tail --
    a power cut mid-write is expected in a car, not an error."""
    with open(path, "rb") as f:
        blob = f.read()
    if not blob.startswith(MAGIC):
        sys.exit(f"{path}: not a CANLOG2 file (bad magic)")
    b1, b2 = struct.unpack_from("<II", blob, 8)
    yield ("HEADER", b1, b2)
    off, n_trunc = HDR, 0
    end = len(blob)
    while off + REC <= end:
        t_us, bus, lf, cid = struct.unpack_from("<IBBI", blob, off)
        ln = lf & 0x0F
        if off + REC + ln > end:
            n_trunc += 1
            break
        data = blob[off + REC: off + REC + ln]
        off += REC + ln
        yield (t_us, bus, cid, bool(lf & 0x10), bool(lf & 0x20), data)
    if n_trunc:
        print(f"note: log ends mid-record (expected after a power cut)", file=sys.stderr)


def load(path):
    recs, b1, b2 = [], None, None
    for r in read_log(path):
        if r[0] == "HEADER":
            _, b1, b2 = r
        else:
            recs.append(r)
    return recs, b1, b2


# ---------------------------------------------------------------- stats
def cmd_stats(args):
    recs, b1, b2 = load(args.file)
    if not recs:
        sys.exit("no records")
    t0, t1 = recs[0][0], recs[-1][0]
    span = max((t1 - t0) / 1e6, 1e-9)
    print(f"file      : {args.file}")
    print(f"bus baud  : bus0={b1} bus1={b2}")
    print(f"records   : {len(recs):,}   span {span:.1f} s   {len(recs)/span:.0f} frames/s")

    per = defaultdict(lambda: {"n": 0, "last": None, "chg": bytearray(8),
                               "dlc": 0, "first_t": None, "last_t": None})
    for t, bus, cid, ext, rem, data in recs:
        e = per[(bus, cid)]
        e["n"] += 1
        e["dlc"] = len(data)
        if e["first_t"] is None:
            e["first_t"] = t
        e["last_t"] = t
        if e["last"] is not None:
            for i in range(min(len(data), len(e["last"]))):
                e["chg"][i] |= data[i] ^ e["last"][i]
        e["last"] = data

    print(f"unique IDs: {len(per)}")
    print()
    print("bus  id     dlc   count   rate/s  live-bits (set = bit has changed)   last bytes")
    for (bus, cid), e in sorted(per.items(), key=lambda kv: -kv[1]["n"]):
        rate = e["n"] / span
        chg = " ".join(f"{e['chg'][i]:02X}" for i in range(e["dlc"]))
        last = " ".join(f"{b:02X}" for b in e["last"])
        print(f"{bus:<4} {cid:03X}    {e['dlc']}  {e['n']:7d}  {rate:7.1f}  {chg:<24}  {last}")


# ------------------------------------------------------------ candidates
def _score(series):
    """Higher = more like a real analog sensor value, less like a counter,
    checksum, or constant. Returns (score, reason)."""
    if len(series) < 20:
        return 0.0, "too few samples"
    uniq = len(set(series))
    if uniq == 1:
        return 0.0, "constant"
    d = [series[i + 1] - series[i] for i in range(len(series) - 1)]
    # unwrap byte rollover so a counter looks like a counter
    d = [(x + 128) % 256 - 128 for x in d]
    nz = [x for x in d if x != 0]
    if not nz:
        return 0.0, "constant"
    small = sum(1 for x in nz if abs(x) <= 2) / len(nz)
    zero_frac = 1.0 - len(nz) / len(d)
    # a free-running counter: almost every delta identical and non-zero
    from collections import Counter
    common, cnt = Counter(nz).most_common(1)[0]
    counterish = cnt / len(nz)
    if counterish > 0.9 and abs(common) <= 2 and zero_frac < 0.1:
        return 0.05, f"counter (delta {common:+d} {counterish:.0%})"
    if uniq > 200 and small < 0.2:
        return 0.02, "random-looking (checksum/crypto?)"
    # A free-running counter changes on EVERY sample; a slow sensor is mostly
    # still. So only penalise repeated-delta when the signal is also active --
    # otherwise a monotonic drift (battery SoC draining, tank emptying, temp
    # rising) scores zero, which is precisely the signal we most want to find.
    counter_penalty = counterish * (1.0 - zero_frac)
    score = small * (1.0 - counter_penalty) * min(uniq / 40.0, 1.0)
    monotonic = " monotonic" if counterish > 0.9 and zero_frac > 0.5 else ""
    return score, f"smooth {small:.0%}, {uniq} levels, still {zero_frac:.0%}{monotonic}"


def cmd_candidates(args):
    recs, _, _ = load(args.file)
    series = defaultdict(list)
    for t, bus, cid, ext, rem, data in recs:
        for i, b in enumerate(data):
            series[(bus, cid, i)].append(b)
    # also try 16-bit big/little pairs -- most real signals are wider than a byte
    wide = {}
    for (bus, cid, i), s in list(series.items()):
        nxt = series.get((bus, cid, i + 1))
        if nxt and len(nxt) == len(s):
            wide[(bus, cid, i, "BE")] = [(a << 8) | b for a, b in zip(s, nxt)]
            wide[(bus, cid, i, "LE")] = [(b << 8) | a for a, b in zip(s, nxt)]

    rows = []
    for (bus, cid, i), s in series.items():
        sc, why = _score(s)
        if sc > 0:
            rows.append((sc, bus, cid, i, 8, "-", why, min(s), max(s)))
    for (bus, cid, i, endian), s in wide.items():
        sc, why = _score([v >> 8 for v in s])  # score on the high byte's smoothness
        sc *= 1.15                              # mild bonus: 16-bit signals are common
        if sc > 0:
            rows.append((sc, bus, cid, i, 16, endian, why, min(s), max(s)))

    rows.sort(reverse=True)
    print("Ranked candidate signals -- bytes that BEHAVE like sensor values.")
    print("Correlate the top ones against what the car was doing when you logged.\n")
    print("score  bus  id   byte  width  end   min     max     why")
    for r in rows[:args.top]:
        sc, bus, cid, i, w, en, why, lo, hi = r
        print(f"{sc:5.2f}  {bus:<3}  {cid:03X}   {i:<4}  {w:<5}  {en:<4}  "
              f"{lo:<6}  {hi:<6}  {why}")
    if not rows:
        print("(nothing scored above zero -- log for longer, or while the value you "
              "care about actually changes)")


# --------------------------------------------------------------- extract
def cmd_extract(args):
    recs, _, _ = load(args.file)
    cid = int(args.id, 16)
    t0 = None
    out = sys.stdout if args.out == "-" else open(args.out, "w")
    print("t_s,raw,scaled", file=out)
    for t, bus, mid, ext, rem, data in recs:
        if mid != cid or (args.bus is not None and bus != args.bus):
            continue
        if t0 is None:
            t0 = t
        b = args.byte
        if args.width == 8:
            if b >= len(data):
                continue
            raw = data[b]
        else:
            if b + 1 >= len(data):
                continue
            raw = ((data[b] << 8) | data[b + 1]) if args.endian == "BE" \
                  else ((data[b + 1] << 8) | data[b])
        if args.signed:
            bits = args.width
            if raw >= (1 << (bits - 1)):
                raw -= (1 << bits)
        print(f"{(t - t0)/1e6:.4f},{raw},{raw * args.scale + args.offset:.4f}", file=out)
    if out is not sys.stdout:
        out.close()
        print(f"wrote {args.out}")


# ------------------------------------------------------------------ dump
def cmd_dump(args):
    recs, _, _ = load(args.file)
    cid = int(args.id, 16) if args.id else None
    t0 = None
    n = 0
    for t, bus, mid, ext, rem, data in recs:
        if cid is not None and mid != cid:
            continue
        if t0 is None:
            t0 = t
        print(f"{(t-t0)/1e6:10.4f}  bus{bus}  {mid:03X}  [{len(data)}]  "
              + " ".join(f"{b:02X}" for b in data))
        n += 1
        if args.limit and n >= args.limit:
            break


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("stats", help="ID census: rates + which bits ever change")
    s.add_argument("file"); s.set_defaults(func=cmd_stats)

    s = sub.add_parser("candidates", help="rank bytes that look like real signals")
    s.add_argument("file"); s.add_argument("--top", type=int, default=30)
    s.set_defaults(func=cmd_candidates)

    s = sub.add_parser("extract", help="pull one signal to CSV")
    s.add_argument("file"); s.add_argument("--id", required=True, help="hex CAN id")
    s.add_argument("--byte", type=int, required=True)
    s.add_argument("--width", type=int, choices=[8, 16], default=8)
    s.add_argument("--endian", choices=["BE", "LE"], default="BE")
    s.add_argument("--signed", action="store_true")
    s.add_argument("--scale", type=float, default=1.0)
    s.add_argument("--offset", type=float, default=0.0)
    s.add_argument("--bus", type=int, default=None)
    s.add_argument("--out", default="-")
    s.set_defaults(func=cmd_extract)

    s = sub.add_parser("dump", help="raw frames")
    s.add_argument("file"); s.add_argument("--id", default=None)
    s.add_argument("--limit", type=int, default=50)
    s.set_defaults(func=cmd_dump)

    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
