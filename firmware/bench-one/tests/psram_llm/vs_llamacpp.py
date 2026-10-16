#!/usr/bin/env python3
"""vs_llamacpp.py -- the reference model's greedy answers against llama.cpp's, same file, same token ids.

    python vs_llamacpp.py <survey dir>          (llama-server on 127.0.0.1:8089 with the same GGUF)

For every survey prompt: the prompt ids and generated ids of tests/tl_ref.exe (the survey's NN.txt, which the
Teensy reproduces) against llama.cpp's greedy completion of the same ids, with the top-2 margin on both sides
at the first difference. llama.cpp quantizes activations its own way (q8_K blocks of 256; tl_ref int8 blocks
of 32), so two correct implementations can part at a near tie; a parting at a wide margin would be a fault.
Writes vs_llamacpp.md into the survey dir. Correctness only; no timing is recorded."""
import json
import os
import re
import sys
import urllib.request

STEP = re.compile(r'^step (\d+) pos \d+ fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"$')


def complete(ids, n):
    body = {"prompt": ids, "n_predict": n, "temperature": 0, "top_k": 1, "n_probs": 2, "cache_prompt": False,
            "return_tokens": True}
    req = urllib.request.Request("http://127.0.0.1:8089/completion", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    d = json.loads(urllib.request.urlopen(req, timeout=600).read())
    margins = []
    for p in d.get("completion_probabilities", []):
        lp = [q.get("logprob") for q in p.get("top_logprobs", [])]
        margins.append(lp[0] - lp[1] if len(lp) > 1 else None)
    return d.get("tokens", []), margins, d.get("content", "")


def main():
    sd = sys.argv[1]
    rows = [json.loads(s) for s in open(os.path.join(sd, "survey.jsonl"), encoding="utf-8")]
    out = ["# The reference model against llama.cpp -- %s" % os.path.basename(sd), "",
           "Greedy, same GGUF, same prompt token ids. `ours` = tests/tl_ref.exe (what the Teensy computes). A parting",
           "is shown with each side's top-2 logit margin at that token.", "",
           "| # | kind | generated | identical tokens | first parting at | margin ours / llama.cpp | ours from there | llama.cpp from there |",
           "|---|---|---|---|---|---|---|---|"]
    full, near = 0, 0
    for r in rows:
        lines = open(os.path.join(sd, "%02d.txt" % r["n"]), encoding="utf-8").read().splitlines()
        pids = [int(x) for x in lines[0].split(":", 1)[1].split()]
        gen, marg, text = [], [], []
        for ln in lines[1:]:
            m = STEP.match(ln)
            if m and int(m.group(1)) >= len(pids) - 1:
                gen.append(int(m.group(3)))
                marg.append(float(m.group(4)) - float(m.group(6)))
                text.append(m.group(7))
        lt, lm, _ = complete(pids, len(gen))
        k = 0
        while k < min(len(gen), len(lt)) and gen[k] == lt[k]:
            k += 1
        same = k == len(gen) == len(lt) or (k == len(lt) and k == len(gen))
        if same:
            full += 1
            out.append("| %d | %s | %d | **all %d** | | | | |" % (r["n"], r["category"], len(gen), len(gen)))
        else:
            mo = marg[k] if k < len(marg) else None
            ml = lm[k] if k < len(lm) and lm[k] is not None else None
            if mo is not None and mo < 0.2:
                near += 1
            out.append("| %d | %s | %d | %d | token %d | %s / %s | %s | %s |" % (
                r["n"], r["category"], len(gen), k, k, "%.3f" % mo if mo is not None else "-",
                "%.3f" % ml if ml is not None else "-", "".join(text[k:k + 4]).replace("|", "/")[:40],
                str(lt[k:k + 4])))
        print("[%02d] %d/%d identical" % (r["n"], k, len(gen)), flush=True)
    out += ["", "**%d of %d answers identical token for token; %d of the %d partings are at a margin under 0.2 on our side.**" % (
        full, len(rows), near, len(rows) - full)]
    open(os.path.join(sd, "vs_llamacpp.md"), "w", encoding="utf-8").write("\n".join(out) + "\n")
    print(out[-1])


if __name__ == "__main__":
    main()
