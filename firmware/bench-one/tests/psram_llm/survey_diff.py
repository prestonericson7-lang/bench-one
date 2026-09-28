#!/usr/bin/env python3
"""survey_diff.py -- the same prompts in two surveys, side by side: what changed and where.

    python survey_diff.py <old survey dir> <new survey dir>

Prompts are matched by text; a chat prompt written out by hand in the old file
("<|im_start|>user\\nX<|im_end|>\\n<|im_start|>assistant\\n") is matched to the new file's "chat | X"."""
import json
import os
import re
import sys

RAW_CHAT = re.compile(r"^<\|im_start\|>user\\n(.*)<\|im_end\|>\\n<\|im_start\|>assistant\\n$")


def load(d):
    out = {}
    for s in open(os.path.join(d, "survey.jsonl"), encoding="utf-8"):
        r = json.loads(s)
        m = RAW_CHAT.match(r["prompt"])
        out[m.group(1) if m else r["prompt"]] = r
    return out


def first_diff(a, b):
    k = 0
    while k < min(len(a), len(b)) and a[k] == b[k]:
        k += 1
    return k


def main():
    old, new = load(sys.argv[1]), load(sys.argv[2])
    both = [p for p in new if p in old]
    same = sum(1 for p in both if old[p]["answer"] == new[p]["answer"])
    print("# Survey against survey: %s -> %s" % (os.path.basename(sys.argv[1]), os.path.basename(sys.argv[2])))
    print()
    print("%d prompts in both; %d answers unchanged, %d changed." % (len(both), same, len(both) - same))
    print()
    print("| prompt | tokens old -> new | answer, old | answer, new | differs from character |")
    print("|---|---|---|---|---|")
    for p in both:
        a, b = old[p]["answer"], new[p]["answer"]
        k = first_diff(a, b)
        cell = lambda s: s.replace("|", "/").replace("\n", "\\n")[:110]
        print("| `%s` | %d -> %d | %s | %s | %s |" % (p.replace("|", "/")[:50], old[p]["n_prompt"], new[p]["n_prompt"],
                                                     cell(a), cell(b), "same" if a == b else str(k)))


if __name__ == "__main__":
    main()
