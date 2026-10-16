#!/usr/bin/env python3
"""vs_llamacpp_moe.py -- the reference (tests/tl_ref.exe, what the Teensy computes) against llama.cpp on a model given by
MODEL=..., greedy, the same prompt token ids. Written 2026-10-16 for Qwen3-Coder-30B-A3B, when model_q.c began summing a
mixture-of-experts layer's eight experts in ascending expert number (the order a batched pass on the board needs) instead
of the order they were picked: the independent implementation says whether the model is still the model.

    MODEL=<the.gguf> python vs_llamacpp_moe.py [--port 8090] [--gen 16] [--out DIR]

Needs llama-server on 127.0.0.1:<port> serving the same GGUF (Ollama ships llama-server.exe in lib/ollama). llama.cpp
quantizes activations its own way (q8_K, blocks of 256; tl_ref int8 blocks of 32), so the two can part where the top
two logits are close; each parting is shown with both sides' margins there. Correctness only, no timing."""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, HERE)
from verify_pc import PROMPTS  # noqa: E402

STEP = re.compile(r'^step (\d+) pos \d+ fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"$')


def complete(port, ids, n):
    body = {"prompt": ids, "n_predict": n, "temperature": 0, "top_k": 1, "n_probs": 2, "cache_prompt": False,
            "return_tokens": True}
    req = urllib.request.Request("http://127.0.0.1:%d/completion" % port, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    d = json.loads(urllib.request.urlopen(req, timeout=1800).read())
    margins = []
    for p in d.get("completion_probabilities", []):
        lp = [q.get("logprob") for q in p.get("top_logprobs", [])]
        margins.append(lp[0] - lp[1] if len(lp) > 1 else None)
    return d.get("tokens", []), margins


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--gen", type=int, default=16)
    ap.add_argument("--out")
    a = ap.parse_args()
    model = os.environ["MODEL"]
    out = a.out or os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-vs-llamacpp-moe")
    os.makedirs(out, exist_ok=True)
    rep = ["# The reference against llama.cpp -- %s" % os.path.basename(model), "",
           "Greedy, same GGUF, same prompt token ids. `ours` = tests/tl_ref.exe (model_q.c's fused path; what the Teensy",
           "computes, and tests/tl_host.exe equals it to the bit). A parting shows each side's top-2 margin there.", "",
           "| # | generated | identical tokens | first parting at | margin ours / llama.cpp | ours from there | llama.cpp from there |",
           "|---|---|---|---|---|---|---|"]
    full = near = 0
    for k, p in enumerate(PROMPTS):
        ref = os.path.join(out, "ref_%d.txt" % k)
        if not os.path.exists(ref):
            with open(ref, "wb") as f:
                subprocess.run([os.path.join(TESTS, "tl_ref.exe"), model, p, str(a.gen)], stdout=f,
                               stderr=subprocess.DEVNULL, cwd=TESTS)
        lines = open(ref, encoding="utf-8").read().splitlines()
        pids = [int(x) for x in lines[0].split(":", 1)[1].split()]
        gen, marg, text = [], [], []
        for ln in lines[1:]:
            m = STEP.match(ln)
            if m and int(m.group(1)) >= len(pids) - 1:
                gen.append(int(m.group(3)))
                marg.append(float(m.group(4)) - float(m.group(6)))
                text.append(m.group(7))
        lt, lm = complete(a.port, pids, len(gen))
        j = 0
        while j < min(len(gen), len(lt)) and gen[j] == lt[j]:
            j += 1
        if j == len(gen) and j == len(lt):
            full += 1
            rep.append("| %d | %d | **all %d** | | | | |" % (k, len(gen), len(gen)))
        else:
            mo = marg[j] if j < len(marg) else None
            ml = lm[j] if j < len(lm) and lm[j] is not None else None
            if mo is not None and mo < 0.2:
                near += 1
            rep.append("| %d | %d | %d | token %d | %s / %s | %s | %s |" % (
                k, len(gen), j, j, "%.3f" % mo if mo is not None else "-", "%.3f" % ml if ml is not None else "-",
                "".join(text[j:j + 4]).replace("|", "/")[:40], str(lt[j:j + 4])))
        print("[%d] %d/%d identical  %r" % (k, j, len(gen), p[:50]), flush=True)
    rep += ["", "**%d of %d answers identical token for token; %d of the %d partings are at a margin under 0.2 on our side.**"
            % (full, len(PROMPTS), near, len(PROMPTS) - full)]
    open(os.path.join(out, "vs_llamacpp.md"), "w", encoding="utf-8").write("\n".join(rep) + "\n")
    print(rep[-1])
    print("archive:", out)


if __name__ == "__main__":
    main()
