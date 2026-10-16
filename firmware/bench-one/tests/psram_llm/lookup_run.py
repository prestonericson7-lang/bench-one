#!/usr/bin/env python3
"""lookup_run.py -- speculative decoding from the context (::lookup) on the board, every answer checked.

    python firmware/bench-one/tests/psram_llm/lookup_run.py [--gen 24] [--k 6] [--cost <passcost serial.log>] <survey dir>/<NN> ...

For each prompt (a survey answer: its exact text and the reference's step lines, tests/tl_ref.exe):
  1. ::gen G, ::lookup K, then the prompt as the survey sent it.
  2. Every step line the board prints must equal the reference's line for that position (ids, both logits to
     nine digits, second token): a guess only ever skips passes, never changes a token.
  3. The board's LOOKUP line (passes, guesses made, guesses kept) against alpha_sim.simulate() for the same
     answer and budget -- the PC prediction, now checked against the board.
  4. Time: the board's own per-pass lines. The plain-decoding time for the same answer is the number of
     tokens after the first times the run's own no-guess pass (the ordinary one-position pass, tl_forward),
     so both sides come from the same board, the same card and the same minute.
Die temperature is logged with every pass; the run stops at PASSCOST_TEMP_STOP (default 75 C).
Everything goes to bench-archive/<stamp>-psram_llm-lookup/{serial.log,summary.md}."""
import argparse
import json
import os
import re
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, HERE)
import alpha_sim  # noqa: E402

PASS = re.compile(r"^    (prefill=\d+\.\.\d+|lookup=\d+ guessed=(\d+) kept=(\d+)|step=\d+): ([0-9.]+) s: SD ([0-9.]+) MB in ([0-9.]+) s")
TEMP = re.compile(r"temp_c[= ]([0-9.]+)")


def escape(s):
    """the inverse of the firmware's unescape(): \\ \n \t"""
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", type=int, default=24)
    ap.add_argument("--k", type=int, default=3)
    ap.add_argument("--nmin", type=int, default=1, help="shortest match: the board's ::lookup K N")
    ap.add_argument("--cost", help="pass_cost.py serial.log, for the predicted time (optional)")
    ap.add_argument("picks", nargs="+", help="<survey dir>/<NN>, e.g. bench-archive/20261016-002448-psram_llm-survey/06")
    A = ap.parse_args()
    cost = alpha_sim.read_cost(A.cost)[0] if A.cost else None
    out = os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-psram_llm-lookup")
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "serial.log"), "a", encoding="utf-8")
    port = next(p.device for p in list_ports.comports() if getattr(p, "vid", None) == 0x16C0)
    s = serial.Serial(port, 115200, timeout=0.5)
    stop_at = float(os.environ.get("PASSCOST_TEMP_STOP", "75"))
    seen = []

    def send(cmd):
        log.write("%s > %s\n" % (time.strftime("%H:%M:%S"), cmd)); log.flush()
        s.write((cmd + "\n").encode())

    def rd_until(pat, limit):
        t0 = time.time()
        while time.time() - t0 < limit:
            line = s.readline().decode("utf-8", "replace").rstrip("\r\n")
            if not line:
                continue
            log.write("%s | %s\n" % (time.strftime("%H:%M:%S"), line)); log.flush()
            seen.append(line)
            tm = TEMP.search(line)
            if tm and float(tm.group(1)) >= stop_at:
                send("::stop")
                sys.exit("die at %s C (limit %.0f): stopped" % (tm.group(1), stop_at))
            m = re.search(pat, line)
            if m:
                return line, m
        return None, None

    send("I")
    if rd_until(r"^I bench-one psram_llm", 60)[0] is None:
        sys.exit("the board did not answer I")
    ident = seen[-1]
    send("::gen %d" % A.gen); rd_until(r"^GEN", 10)
    send("::lookup %d %d" % (A.k, A.nmin)); rd_until(r"^LOOKUP %d" % A.k, 10)
    rows = []
    for pk in A.picks:
        d, nn = os.path.split(pk.rstrip("/"))
        d = d if os.path.isabs(d) else os.path.join(ROOT, d)
        rec = [json.loads(l) for l in open(os.path.join(d, "survey.jsonl"), encoding="utf-8") if json.loads(l)["n"] == int(nn)][0]
        ref = [l.rstrip("\n") for l in open(os.path.join(d, "%02d.txt" % int(nn)), encoding="utf-8") if l.startswith("step ")]
        _, cat, p, g = [it for it in alpha_sim.load(d) if it[0].endswith("/%02d" % int(nn))][0]
        n0 = len(seen)
        send(escape(rec["sent"]))
        line, _ = rd_until(r"^RUN ", (len(p) // 8 + 2 + A.gen) * 300)
        if not line:
            sys.exit("no RUN line for %s" % pk)
        part = seen[n0:]
        steps = [re.sub(r"^R ", "", l) for l in part if l.startswith("step ")]
        want = min(len(ref), len(p) + A.gen)            # positions 0..n+gen-1, or fewer if the answer ends first
        same = len(steps) == want and steps == ref[:want]
        lk = [l for l in part if l.startswith("LOOKUP k=")]
        m = re.search(r"passes=(\d+) drafted=(\d+) accepted=(\d+)", lk[0]) if lk else None
        b_passes, b_drafted, b_acc = map(int, m.groups()) if m else (-1, -1, -1)
        prompt_s = look_s = 0.0
        plain_pass = []
        npass = {}
        for l in part:
            pm = PASS.match(l)
            if not pm:
                continue
            t = float(pm.group(4))
            if pm.group(1).startswith("prefill"):
                prompt_s += t
            else:
                look_s += t
                if pm.group(2) is not None:
                    P = int(pm.group(2)) + 1
                    npass[P] = npass.get(P, 0) + 1
                    if P == 1:
                        plain_pass.append(t)
        temps = [float(x) for x in TEMP.findall("\n".join(part))]
        gen_tokens = len(steps) - len(p)                  # positions fed after the prompt = tokens after the first
        one = sorted(plain_pass)[len(plain_pass) // 2] if plain_pass else 104.8
        base = gen_tokens * one
        gg = g[:A.gen + 1]
        if 151645 in gg:
            gg = gg[:gg.index(151645) + 1]
        sim = alpha_sim.simulate(p, gg, 4, A.nmin, A.k, A.gen)
        pred = (len(sim), sum(x[2] for x in sim), sum(x[3] for x in sim))
        pt = sum(cost[P] for P, *_ in sim) if cost else None
        rows.append(dict(pick=pk, cat=cat, prompt=rec["prompt"], n=len(p), steps=len(steps), ref=len(ref), same=same,
                         board=(b_passes, b_drafted, b_acc), pred=pred, prompt_s=prompt_s, look_s=look_s, base=base,
                         one=one, plain_n=len(plain_pass), gen=gen_tokens, npass=npass, pt=pt,
                         tmax=max(temps) if temps else float("nan")))
        r = rows[-1]
        print("%s: %d step lines %s; board passes/guessed/kept %s, predicted %s; generation %.1f s vs plain %.1f s (%.3fx); die max %.1f C"
              % (pk, r["steps"], "IDENTICAL to the reference" if same else "DIFFERENT from the reference", r["board"], r["pred"],
                 look_s, base, base / look_s if look_s else 0, r["tmax"]), flush=True)
    sm = ["# Speculative decoding from the context on the board (::lookup %d %d, ::gen %d)\n" % (A.k, A.nmin, A.gen),
          "`%s`\n" % ident,
          "Each pass feeds the pending token plus up to %d tokens guessed from the context (the tokens that followed the "
          "most recent earlier occurrence of the last 4 down to %d tokens); the board keeps the guesses only as far as its own "
          "greedy choice agrees. Plain decoding = tokens after the first x the run's own no-guess pass (the ordinary "
          "one-position pass).\n" % (A.k, A.nmin),
          "| prompt | kind | prompt tokens | step lines vs reference | passes / guessed / kept (board) | predicted (alpha_sim) | "
          "generation s | plain s | speed-up | passes by positions | die max C |",
          "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        sm.append("| `%s` | %s | %d | %s (%d) | %d / %d / %d | %d / %d / %d%s | %.1f | %.1f | %.3fx | %s | %.1f |" % (
            r["prompt"][:48].replace("|", "\\|"), r["cat"], r["n"], "IDENTICAL" if r["same"] else "**DIFFERENT**", r["steps"],
            *r["board"], *r["pred"], "" if r["board"] == r["pred"] else " **(differs)**", r["look_s"], r["base"],
            r["base"] / r["look_s"] if r["look_s"] else 0, ", ".join("%dx%d" % (v, k) for k, v in sorted(r["npass"].items())),
            r["tmax"]))
    tl = sum(r["look_s"] for r in rows); tb = sum(r["base"] for r in rows)
    sm.append("\nAll prompts: generation %.1f s against %.1f s plain: **%.3fx**. Every step line %s the reference." % (
        tl, tb, tb / tl if tl else 0, "matches" if all(r["same"] for r in rows) else "does NOT match"))
    sm.append("Board counts %s the PC prediction for every prompt." % (
        "equal" if all(r["board"] == r["pred"] for r in rows) else "do NOT all equal"))
    text = "\n".join(sm) + "\n"
    open(os.path.join(out, "summary.md"), "w", encoding="utf-8").write(text)
    print(text)
    print("archive:", out)


if __name__ == "__main__":
    main()
