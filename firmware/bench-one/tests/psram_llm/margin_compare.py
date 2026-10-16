#!/usr/bin/env python3
"""margin_compare.py -- how far apart three implementations' logits are, step by step, on the survey prompts.

    python margin_compare.py <survey dir>         (llama-server on 127.0.0.1:8089 with the same GGUF)

At every generated step where fast (tests/tl_ref.exe, what the Teensy computes), float (TL_REF_FLOAT=1) and
llama.cpp still share the whole context, take fast's top-1 and top-2 tokens and that pair's logit difference
in each implementation (llama.cpp: difference of their log-probabilities, which is the logit difference; only
where both tokens are in its top 10). Same token pair everywhere, so the numbers compare exactly.

It separates two explanations of fast == float != llama.cpp: llama.cpp's own quantization noise (differences
spread evenly, no growth with position) or a shared bug in model_q.c's non-matrix code, which both of our
paths run (differences systematic, growing along the sequence). Writes margin_compare.md. Correctness only."""
import json
import os
import re
import statistics
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
MODEL = "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba"
STEP = re.compile(r'^step (\d+) pos \d+ fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"$')


def parse(lines):
    pids = [int(x) for x in lines[0].split(":", 1)[1].split()]
    st = []
    for ln in lines[1:]:
        m = STEP.match(ln)
        if m and int(m.group(1)) >= len(pids) - 1:
            st.append((int(m.group(1)), int(m.group(3)), float(m.group(4)), int(m.group(5)), float(m.group(6))))
    return pids, st


def llama(ids, n):
    body = {"prompt": ids, "n_predict": n, "temperature": 0, "top_k": 1, "n_probs": 10, "cache_prompt": False,
            "return_tokens": True}
    req = urllib.request.Request("http://127.0.0.1:8089/completion", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    d = json.loads(urllib.request.urlopen(req, timeout=600).read())
    return d.get("tokens", []), [{q["id"]: q["logprob"] for q in p.get("top_logprobs", [])}
                                 for p in d.get("completion_probabilities", [])]


def stats(xs):
    if not xs:
        return "-"
    s = sorted(xs)
    return "%.4f / %.4f / %.4f / %.4f (n=%d)" % (statistics.mean(s), s[len(s) // 2], s[int(len(s) * 0.95)], s[-1], len(s))


def main():
    sd = sys.argv[1]
    rows = [json.loads(s) for s in open(os.path.join(sd, "survey.jsonl"), encoding="utf-8")]
    recs = []
    for r in rows:
        pids, fast = parse(open(os.path.join(sd, "%02d.txt" % r["n"]), encoding="utf-8").read().splitlines())
        fpath = os.path.join(sd, "float_%02d.txt" % r["n"])
        if not os.path.exists(fpath):
            out = subprocess.run([os.path.join(TESTS, "tl_ref.exe"), MODEL, r["sent"], str(len(fast) - 1)],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=TESTS,
                                 env=dict(os.environ, TL_REF_FLOAT="1"))
            open(fpath, "wb").write(out.stdout.replace(b"\r\n", b"\n"))
        _, flo = parse(open(fpath, encoding="utf-8").read().splitlines())
        lt, lp = llama(pids, len(fast))
        for k in range(min(len(fast), len(flo), len(lt), len(lp))):
            pos, t1, l1, t2, l2 = fast[k]
            _, ft1, fl1, ft2, fl2 = flo[k]
            rec = dict(n=r["n"], pos=pos, step=k, fast=l1 - l2)
            if (ft1, ft2) == (t1, t2):
                rec["float"] = fl1 - fl2
            if t1 in lp[k] and t2 in lp[k]:
                rec["llama"] = lp[k][t1] - lp[k][t2]
            recs.append(rec)
            if not (ft1 == t1 and lt[k] == t1):       # contexts part after this step
                break
        print("[%02d] %d steps compared" % (r["n"], sum(1 for x in recs if x["n"] == r["n"])), flush=True)
    json.dump(recs, open(os.path.join(sd, "margin_compare.json"), "w"))
    pairs = {"fast - float": [abs(x["fast"] - x["float"]) for x in recs if "float" in x],
             "fast - llama.cpp": [abs(x["fast"] - x["llama"]) for x in recs if "llama" in x],
             "float - llama.cpp": [abs(x["float"] - x["llama"]) for x in recs if "float" in x and "llama" in x]}
    md = ["# Logit differences between implementations -- %s" % os.path.basename(sd), "",
          "Same context, same token pair (fast's top-1 and top-2): |difference of that pair's logit gap| between",
          "implementations. mean / median / 95th percentile / max.", "", "| pair | all steps |", "|---|---|"]
    for k, v in pairs.items():
        md.append("| %s | %s |" % (k, stats(v)))
    md += ["", "By absolute position in the sequence (does it grow?):", "",
           "| positions | fast - float | fast - llama.cpp | float - llama.cpp |", "|---|---|---|---|"]
    for lo, hi in ((0, 10), (10, 20), (20, 40), (40, 60), (60, 200)):
        sel = [x for x in recs if lo <= x["pos"] < hi]
        md.append("| %d-%d | %s | %s | %s |" % (lo, hi - 1,
                  stats([abs(x["fast"] - x["float"]) for x in sel if "float" in x]),
                  stats([abs(x["fast"] - x["llama"]) for x in sel if "llama" in x]),
                  stats([abs(x["float"] - x["llama"]) for x in sel if "float" in x and "llama" in x])))
    open(os.path.join(sd, "margin_compare.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    print("\n".join(md))


if __name__ == "__main__":
    main()
