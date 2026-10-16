#!/usr/bin/env python3
"""cache_split.py -- where the fast path's partings from float come from: the int8 cache or int8 activations.

    python cache_split.py <survey dir> <tl_ref binary with TL_REF_KVF32> [prompt numbers...]

For each prompt: fast (the survey's NN.txt), fast + float cache (TL_REF_KVF32=1), float (float_NN.txt from
margin_compare.py). If fast+float-cache equals float where fast did not, the cache made the difference; if it
still equals fast, the activations did. Writes cache_split.md. Correctness only."""
import json
import os
import re
import subprocess
import sys

MODEL = "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba"
STEP = re.compile(r'^step (\d+) pos \d+ fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"$')


def gen(lines):
    pids = [int(x) for x in lines[0].split(":", 1)[1].split()]
    return [int(m.group(3)) for m in map(STEP.match, lines[1:]) if m and int(m.group(1)) >= len(pids) - 1]


def part(a, b):
    k = 0
    while k < min(len(a), len(b)) and a[k] == b[k]:
        k += 1
    return None if k == len(a) == len(b) else k


def main():
    sd, exe = sys.argv[1], os.path.abspath(sys.argv[2])
    rows = {r["n"]: r for r in map(json.loads, open(os.path.join(sd, "survey.jsonl"), encoding="utf-8"))}
    want = [int(x) for x in sys.argv[3:]] or sorted(rows)
    md = ["# Int8 cache or int8 activations? -- %s" % os.path.basename(sd), "",
          "First differing generated token against float (`same` = identical throughout).", "",
          "| # | fast vs float | fast + float cache vs float | fast + float cache vs fast |", "|---|---|---|---|"]
    for n in want:
        r = rows[n]
        fast = gen(open(os.path.join(sd, "%02d.txt" % n), encoding="utf-8").read().splitlines())
        flo = gen(open(os.path.join(sd, "float_%02d.txt" % n), encoding="utf-8").read().splitlines())
        out = subprocess.run([exe, MODEL, r["sent"], str(len(fast) - 1)], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, cwd=os.path.dirname(exe), env=dict(os.environ, TL_REF_KVF32="1"))
        kv = gen(out.stdout.decode("utf-8", "replace").replace("\r\n", "\n").splitlines())
        f = lambda x: "same" if x is None else "token %d" % x
        md.append("| %d | %s | %s | %s |" % (n, f(part(fast, flo)), f(part(kv, flo)), f(part(kv, fast))))
        print(md[-1], flush=True)
    open(os.path.join(sd, "cache_split.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")


if __name__ == "__main__":
    main()
