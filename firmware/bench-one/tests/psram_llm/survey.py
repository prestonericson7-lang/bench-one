#!/usr/bin/env python3
"""survey.py -- what this 3B model does well, measured on the PC reference that the Teensy reproduces.

    python firmware/bench-one/tests/psram_llm/survey.py [prompts file] [tokens]

The Teensy computes the same tokens as tests/tl_ref.exe (shared/model_q.c, fused path) -- proven step by step,
docs/54 -- so WHAT the model says can be surveyed here in seconds per prompt instead of hours. What this cannot
say anything about is the board's speed; those numbers come only off the board (suite.py).

One thing it measures that matters for the board: the MARGIN between the reference's top two logits at every
generated step. The board's logits drift from the PC's by up to ~0.1 (docs/54), so a step with a smaller margin
is one where the board may legitimately pick the other token. min_margin says how exposed each prompt is.

Output: bench-archive/<stamp>-psram_llm-survey/{survey.md, survey.jsonl, <n>.txt}."""
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
MODEL = "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba"
STEP = re.compile(r'^step (\d+) pos \d+ fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"$')
sys.path.insert(0, HERE)
from suite import unescape  # noqa: E402  the firmware's exact unescape
import chat_template  # noqa: E402


def decode_esc(s):
    """Undo the step line's escaping: \\" \\\\ and \\xHH."""
    out, i = bytearray(), 0
    b = s.encode("utf-8", "replace")
    while i < len(b):
        if b[i:i + 1] == b"\\" and i + 1 < len(b):
            if b[i + 1:i + 2] == b"x" and i + 3 < len(b):
                out.append(int(b[i + 2:i + 4], 16)); i += 4; continue
            out.append(b[i + 1]); i += 2; continue
        out.append(b[i]); i += 1
    return out.decode("utf-8", "replace")


def main():
    pf = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "survey_prompts.txt")
    ntok = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    outdir = os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-psram_llm-survey")
    os.makedirs(outdir, exist_ok=True)
    rows = []
    items = []
    for raw in open(pf, encoding="utf-8"):
        s = raw.rstrip("\n")
        if not s.strip() or s.startswith("#"):
            continue
        cat, prompt = [x.strip() if i == 0 else x.lstrip() for i, x in enumerate(s.split("|", 1))]
        items.append((cat, prompt))
    jf = open(os.path.join(outdir, "survey.jsonl"), "w", encoding="utf-8")
    for k, (cat, prompt) in enumerate(items, 1):
        text = unescape(prompt)
        if cat == "chat" and "<|im_start|>" not in text:
            text = chat_template.render(text)        # the model's own template, system turn and all
        out = os.path.join(outdir, "%02d.txt" % k)
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            subprocess.run([os.path.join(TESTS, "tl_ref.exe"), MODEL, text, str(ntok)], stdout=f,
                           stderr=subprocess.DEVNULL, cwd=TESTS)
        lines = open(out, encoding="utf-8").read().splitlines()
        n_prompt = len(lines[0].split(":", 1)[1].split()) if lines and lines[0].startswith("prompt ids:") else 0
        gen, margins = [], []
        for ln in lines:
            m = STEP.match(ln)
            if not m:
                continue
            step = int(m.group(1))
            if step >= n_prompt - 1:
                gen.append(decode_esc(m.group(7)))
                margins.append(float(m.group(4)) - float(m.group(6)))
        answer = "".join(gen)
        r = dict(n=k, category=cat, prompt=prompt, sent=text, n_prompt=n_prompt, n_gen=len(gen), answer=answer,
                 min_margin=min(margins) if margins else None,
                 near_ties=sum(1 for x in margins if x < 0.15))
        rows.append(r)
        jf.write(json.dumps(r, ensure_ascii=False) + "\n"); jf.flush()
        print("[%02d] %-8s min margin %.4f  %r" % (k, cat, r["min_margin"] or 0, answer[:90]), flush=True)
    md = ["# What Qwen2.5-Coder-3B says -- PC reference, identical computation to the Teensy", "",
          "Greedy, %d tokens. min margin = smallest top1-top2 logit gap over the generated steps; below ~0.1 the "
          "board may legitimately choose the other token (docs/54). near ties = steps under 0.15." % ntok, "",
          "| # | kind | prompt | answer | min margin | near ties |", "|---|---|---|---|---|---|"]
    for r in rows:
        md.append("| %d | %s | `%s` | %s | %.4f | %d |" % (
            r["n"], r["category"], r["prompt"].replace("|", "/"),
            r["answer"].replace("|", "/").replace("\n", "\\n")[:160], r["min_margin"] or 0, r["near_ties"]))
    open(os.path.join(outdir, "survey.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    print("wrote", os.path.join(outdir, "survey.md"))


if __name__ == "__main__":
    main()
