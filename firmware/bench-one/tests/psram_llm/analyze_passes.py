#!/usr/bin/env python3
"""analyze_passes.py -- what one pass over the weights costs on the board, from a psram_llm serial.log.

    python analyze_passes.py bench-archive/<stamp>-psram_llm-suite/serial.log [> passes.md]

Every pass prints one line (psram_llm_board.inc, pass_lines):
    "    <what>: T s: SD M MB in S s (R MB/s), PSRAM K KB read W KB written in P s, compute C s; ..."
    "STAGES <what> embed=.. qkv=.. att=.. wo=.. ffn=.. head=.. temp_c=.."
where <what> is "step=N" (one position), "prefill=A..B" (B-A+1 positions of one prompt) or
"multi pass=N slots=K" (K positions of several prompts). Positions per pass = slots.

Out: passes grouped by slots (mean and spread of total, card, PSRAM, M7), and a least-squares line
    time = fixed + per_slot * slots
for the total and for the M7 share -- the measured version of docs/54's projection (97.9 s + 29.6 s/slot).
Only numbers the board printed are used.

v7 reads the card several ways on one image (::sdio, ::overlap, ::align, ::slice). Each pass is tagged with
the way in force when it ran, taken from the board's own lines -- the boot line "SD up on attempt N
(DMA_SDIO)", the identity line, and the replies SDIO / OVERLAP / ALIGN / SLICE -- and every table and fit is
per way, so no line mixes passes read differently. v6 and older read FIFO only."""
import os
import re
import statistics
import sys

PASS = re.compile(r'^\s+(step=\d+|prefill=(\d+)\.\.(\d+)|multi pass=\d+ slots=(\d+)): ([\d.]+) s: SD ([\d.]+) MB in ([\d.]+) s '
                  r'\(([\d.]+) MB/s\), PSRAM (\d+) KB read ([\d.]+) KB written in ([\d.]+) s, compute ([\d.]+) s')
STAGES = re.compile(r'^STAGES (step=\d+|prefill=\d+\.\.\d+|multi pass=\d+ slots=\d+) embed=([\d.]+) qkv=([\d.]+) '
                    r'att=([\d.]+) wo=([\d.]+) ffn=([\d.]+) head=([\d.]+)(?: hidden=([\d.]+))? temp_c=([-\d.]+)')


def fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    a = my - b * mx
    res = [y - (a + b * x) for x, y in zip(xs, ys)]
    return a, b, max(abs(r) for r in res)


BOOT = re.compile(r'^\s+SD up on attempt \d+ \((DMA|FIFO)_SDIO\)')
IDENT = re.compile(r'^I bench-one psram_llm v(\d+) .*?(?: sdio (dma|fifo) overlap (\d)(?: align (\d) slice (\d+)(?: clk (\d+))?)?)?(?: FAKE)?$')


def way_name(w):
    clk = ", card clock %.1f MHz" % (w["khz"] / 1000.0) if w.get("khz") else ""
    if w["sdio"] == "fifo":
        return "FIFO" + clk
    if w["sdio"] == "?":
        return "card mode not in the log"
    return "%s, overlap %d, align %d, slice %s%s%s" % ("ADMA2" if w.get("sdpath") == "adma" else "DMA", w["overlap"], w["align"],
                                                       "%d us" % w["slice"] if w["slice"] else "one row",
                                                       ", bursts " + w["burst"] if w.get("burst") else "", clk)


def main():
    rows, stages = [], []
    w = dict(sdio="?", overlap=1, align=1, slice=0)
    for raw in open(sys.argv[1], encoding="utf-8", errors="replace"):
        s = raw.split(" | ", 1)[1] if " | " in raw[:12] else raw   # suite.py prefixes "HH:MM:SS | "
        s = s.rstrip("\r\n")
        m = BOOT.match(s)
        if m:                                                      # a boot: v7's defaults, in the mode it came up in
            w = dict(sdio=m.group(1).lower(), overlap=1, align=1, slice=0)
            continue
        m = IDENT.match(s)
        if m:                                                      # the identity line does not state the card clock or
            keep = {k: w[k] for k in ("khz", "burst") if k in w}   # the DMA bursts: those carry over (set before it at boot)
            if int(m.group(1)) < 7:
                w = dict(sdio="fifo", overlap=0, align=0, slice=0)
            elif m.group(2):
                if m.group(6):
                    keep["khz"] = int(m.group(6))
                w = dict(sdio=m.group(2), overlap=int(m.group(3)), align=int(m.group(4) or 1), slice=int(m.group(5) or 0), **keep)
            continue
        m = re.match(r'^SDIO (dma|fifo)$', s)
        if m:
            w = dict(w, sdio=m.group(1))
            continue
        m = re.match(r'^(?:CLKSWEEP best khz|SDCLK khz) (\d+)', s)
        if m:                                                      # v8b: the card clock in force (kHz)
            w = dict(w, khz=int(m.group(1)))
            continue
        if re.match(r'^CLKSWEEP best is SdFat', s):
            w = dict(w, khz=49500)
            continue
        m = re.match(r'^(?:SDCFG (set|sdfat)|SWEEP best) wml (\d+) brst (\d+) blen (\d+)', s)
        if m:                                                      # v8b: the DMA burst setting in force
            w = dict(w, burst="" if m.group(1) == "sdfat" else "wml %s brst %s blen %s" % (m.group(2), m.group(3), m.group(4)))
            continue
        m = re.match(r'^SDPATH (file|adma)', s)
        if m:                                                      # v9b: pipeline reads by ADMA2 descriptors
            w = dict(w, sdpath=m.group(1))
            continue
        m = re.match(r'^(OVERLAP|ALIGN|SLICE) (\d+)', s)
        if m:
            w = dict(w, **{m.group(1).lower(): int(m.group(2))})
            continue
        m = PASS.match(s)
        if m:
            if m.group(1).startswith("step"):
                k = 1
            elif m.group(1).startswith("prefill"):
                k = int(m.group(3)) - int(m.group(2)) + 1
            else:
                k = int(m.group(4))
            rows.append(dict(way=way_name(w), kind=m.group(1).split("=")[0].split()[0], slots=k, total=float(m.group(5)),
                             sd_mb=float(m.group(6)), sd_s=float(m.group(7)), sd_mbs=float(m.group(8)),
                             ps_s=float(m.group(11)), cmp=float(m.group(12))))
            continue
        m = STAGES.match(s)
        if m:
            stages.append(dict(way=way_name(w), what=m.group(1), embed=float(m.group(2)), qkv=float(m.group(3)), att=float(m.group(4)),
                               wo=float(m.group(5)), ffn=float(m.group(6)), head=float(m.group(7)), hidden=float(m.group(8) or 0), temp=float(m.group(9))))
    if not rows:
        sys.exit("no pass lines in %s" % sys.argv[1])
    print("# Passes over the weights -- %s" % sys.argv[1])
    print()
    if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "FAKE")):
        print("**DRY RUN against fake_board.py: these timings are invented, not measured.**")
        print()
    print("%d passes: %d one-position, %d batched-prompt, %d multi. Every number below was printed by the board." % (
        len(rows), sum(r["kind"] == "step" for r in rows), sum(r["kind"] == "prefill" for r in rows),
        sum(r["kind"] == "multi" for r in rows)))
    print()
    ways = []
    for r in rows:
        if r["way"] not in ways:
            ways.append(r["way"])
    all_rows, all_stages = rows, stages
    for way in ways:
        rows = [r for r in all_rows if r["way"] == way]
        stages = [x for x in all_stages if x["way"] == way]
        print("## Card read: %s -- %d passes" % (way, len(rows)))
        print()
        print("| slots | passes | total s (mean, min-max) | card s | card MB/s | PSRAM s | M7 s | M7 s per slot |")
        print("|---|---|---|---|---|---|---|---|")
        for k in sorted({r["slots"] for r in rows}):
            g = [r for r in rows if r["slots"] == k]
            t = [r["total"] for r in g]
            print("| %d | %d | %.1f (%.1f-%.1f) | %.1f | %.2f | %.2f | %.1f | %.2f |" % (
                k, len(g), statistics.mean(t), min(t), max(t), statistics.mean(r["sd_s"] for r in g),
                statistics.mean(r["sd_mbs"] for r in g), statistics.mean(r["ps_s"] for r in g),
                statistics.mean(r["cmp"] for r in g), statistics.mean(r["cmp"] for r in g) / k))
        print()
        xs = [r["slots"] for r in rows]
        for label, key in (("total", "total"), ("M7 arithmetic", "cmp"), ("card", "sd_s")):
            f = fit(xs, [r[key] for r in rows])
            if f:
                print("- %s = %.2f s + %.2f s x slots (largest residual %.2f s)" % (label, f[0], f[1], f[2]))
            else:
                print("- %s: every pass had the same slot count, no line to fit" % label)
        if stages:
            print()
            print("Stage split (mean seconds per pass, each stage including its own card reads):")
            print()
            print("| slots | passes | embed | q,k,v | attention | out proj | feed-forward | head | arithmetic under card reads | max C |")
            print("|---|---|---|---|---|---|---|---|---|---|")

            def slots_of(w):
                m = re.search(r"slots=(\d+)", w) or re.search(r"prefill=(\d+)\.\.(\d+)", w)
                if not m:
                    return 1
                return int(m.group(1)) if "slots" in w else int(m.group(2)) - int(m.group(1)) + 1
            for k in sorted({slots_of(s["what"]) for s in stages}):
                g = [s for s in stages if slots_of(s["what"]) == k]
                print("| %d | %d | %s | %.1f |" % (k, len(g), " | ".join("%.2f" % statistics.mean(s[f] for s in g)
                                                                         for f in ("embed", "qkv", "att", "wo", "ffn", "head", "hidden")),
                                                 max(s["temp"] for s in g)))
        print()


if __name__ == "__main__":
    main()
