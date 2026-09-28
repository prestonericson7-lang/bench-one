#!/usr/bin/env python3
"""three_way.py -- fast path (what the Teensy computes), float path, and llama.cpp, on the survey's prompts.

    python three_way.py <survey dir>            (llama-server on 127.0.0.1:8089 with the same GGUF)

fast  = tests/tl_ref.exe (model_q fused int8 path, int8 attention cache) -- the survey's NN.txt
float = tests/tl_ref.exe with TL_REF_FLOAT=1 (weights dequantized to float, float cache)
llama = llama.cpp greedy on the same token ids (its own q8_K activations, f16 cache)
For each pair: how many answers are identical token for token, and where the rest first part.
Writes three_way.md and three_way.jsonl into the survey dir. Correctness only."""
import json
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
MODEL = "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba"
STEP = re.compile(r'^step (\d+) pos \d+ fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"$')


def parse(lines):
    pids = [int(x) for x in lines[0].split(":", 1)[1].split()]
    gen, marg = [], []
    for ln in lines[1:]:
        m = STEP.match(ln)
        if m and int(m.group(1)) >= len(pids) - 1:
            gen.append(int(m.group(3)))
            marg.append(float(m.group(4)) - float(m.group(6)))
    return pids, gen, marg


def llama(ids, n):
    body = {"prompt": ids, "n_predict": n, "temperature": 0, "top_k": 1, "n_probs": 2, "cache_prompt": False,
            "return_tokens": True}
    req = urllib.request.Request("http://127.0.0.1:8089/completion", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    d = json.loads(urllib.request.urlopen(req, timeout=600).read())
    marg = []
    for p in d.get("completion_probabilities", []):
        lp = [q.get("logprob") for q in p.get("top_logprobs", [])]
        marg.append(lp[0] - lp[1] if len(lp) > 1 else None)
    return d.get("tokens", []), marg


def part(a, b):
    k = 0
    while k < min(len(a), len(b)) and a[k] == b[k]:
        k += 1
    return None if (k == len(a) and k == len(b)) else k


def main():
    sd = sys.argv[1]
    rows = [json.loads(s) for s in open(os.path.join(sd, "survey.jsonl"), encoding="utf-8")]
    res = []
    jf = open(os.path.join(sd, "three_way.jsonl"), "w", encoding="utf-8")
    for r in rows:
        pids, fast, fm = parse(open(os.path.join(sd, "%02d.txt" % r["n"]), encoding="utf-8").read().splitlines())
        env = dict(os.environ, TL_REF_FLOAT="1")
        out = subprocess.run([os.path.join(TESTS, "tl_ref.exe"), MODEL, r["sent"], str(len(fast) - 1)],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=TESTS, env=env)
        fl_lines = out.stdout.decode("utf-8", "replace").replace("\r\n", "\n").splitlines()
        fpids, flo, flm = parse(fl_lines)
        lt, lm = llama(pids, len(fast))
        x = dict(n=r["n"], kind=r["category"], prompt_same=(fpids == pids), n_gen=len(fast),
                 fast_float=part(fast, flo), fast_llama=part(fast, lt), float_llama=part(flo, lt))
        for key, (a, am, b, bm) in (("fast_float", (fast, fm, flo, flm)), ("fast_llama", (fast, fm, lt, lm)),
                                    ("float_llama", (flo, flm, lt, lm))):
            k = x[key]
            if k is not None:
                x[key + "_margins"] = [am[k] if k < len(am) else None, bm[k] if k < len(bm) else None]
        res.append(x)
        jf.write(json.dumps(x) + "\n")
        jf.flush()
        print("[%02d] fast/float %s  fast/llama %s  float/llama %s" % (r["n"], x["fast_float"], x["fast_llama"], x["float_llama"]), flush=True)
    n = len(res)
    same = {k: sum(1 for x in res if x[k] is None) for k in ("fast_float", "fast_llama", "float_llama")}
    md = ["# Fast path, float path, llama.cpp -- %s" % os.path.basename(sd), "",
          "%d prompts, greedy, same token ids. Identical token for token over every generated token:" % n, "",
          "| pair | identical answers |", "|---|---|",
          "| fast (the Teensy) vs float | %d of %d |" % (same["fast_float"], n),
          "| fast (the Teensy) vs llama.cpp | %d of %d |" % (same["fast_llama"], n),
          "| float vs llama.cpp | %d of %d |" % (same["float_llama"], n), "",
          "Where they part: first differing generated token, and the two sides' top-2 margins there.", "",
          "| # | kind | tokens | fast/float | fast/llama | float/llama |", "|---|---|---|---|---|---|"]

    def cell(x, k):
        if x[k] is None:
            return "same"
        m = x.get(k + "_margins", [None, None])
        f = lambda v: "-" if v is None else "%.3f" % v
        return "%d (%s/%s)" % (x[k], f(m[0]), f(m[1]))
    for x in res:
        md.append("| %d | %s | %d | %s | %s | %s |" % (x["n"], x["kind"], x["n_gen"], cell(x, "fast_float"),
                                                     cell(x, "fast_llama"), cell(x, "float_llama")))
    open(os.path.join(sd, "three_way.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    print("\n".join(md[4:9]))


if __name__ == "__main__":
    main()
