#!/usr/bin/env python3
"""m7_rerun.py -- verify_pc.py's stage 4 again, alone: the Teensy's own kernels (tests/tl_host_m7.exe, built with
-DGD_EMULATE_M7 from the current tree) through whole forward passes, against the references a finished verify_pc run
left in its archive. For a kernel-only change made after that run (2026-10-16: the batched Q4_K loop's lane words by
mask and no pre-allocation scheduling; the Q6_K tail regrouped exactly) -- the native stages do not run the M7 code.

    python m7_rerun.py <pcverify archive dir> [--model GGUF] [--gen 12] [--prompts 8]

Per token and prompt-batched on prompt 0, then every prompt the archive has a reference for answered together, in
groups of the model's batch. Exits non-zero on any difference."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import chat_template  # noqa: E402
from verify_pc import PROMPTS  # noqa: E402


def same(a, b):
    return open(a, "rb").read().replace(b"\r\n", b"\n") == open(b, "rb").read().replace(b"\r\n", b"\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arch")
    ap.add_argument("--model", default=os.environ.get("MODEL", "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba"))
    ap.add_argument("--gen", type=int, default=12)
    a = ap.parse_args()
    hm = os.path.join(TESTS, "tl_host_m7.exe")
    n_ref = len([f for f in os.listdir(a.arch) if re.match(r"ref_\d+\.txt$", f)])
    prompts = PROMPTS[:n_ref]
    out = os.path.join(a.arch, "m7rerun")
    os.makedirs(out, exist_ok=True)
    fails = 0
    for mode, env in (("per token", {}), ("prompt batched", {"TL_PREFILL": "1"})):
        got = os.path.join(out, "m7_%s.txt" % ("b" if env else "t"))
        e = dict(os.environ, **env)
        with open(got, "wb") as f:
            r = subprocess.run([hm, a.model, prompts[0], str(a.gen)], stdout=f, stderr=subprocess.PIPE, cwd=TESTS, env=e)
        ok = r.returncode == 0 and same(os.path.join(a.arch, "ref_0.txt"), got)
        fails += 0 if ok else 1
        print("tl_host_m7, prompt 0, %s: %s" % (mode, "IDENTICAL" if ok else "DIFFERENT"), flush=True)
    probe = subprocess.run([hm, a.model, prompts[0], "0"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, cwd=TESTS)
    mb = re.search(r"batch (\d+)", probe.stderr.decode("utf-8", "replace"))
    B = int(mb.group(1)) if mb else 8
    n_ok = 0
    for g0 in range(0, len(prompts), B):
        grp = list(range(g0, min(g0 + B, len(prompts))))
        mf = os.path.join(out, "multi_g%d.txt" % g0)
        with open(mf, "w", encoding="utf-8", newline="\n") as f:
            for k in grp:
                f.write(chat_template.escape(prompts[k]) + "\n")
        pre = os.path.join(out, "m7_multi_g%d_" % g0)
        e = dict(os.environ, TL_MULTI=mf, TL_MULTI_OUT=pre)
        r = subprocess.run([hm, a.model, "-", str(a.gen)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, cwd=TESTS, env=e)
        for j, k in enumerate(grp):
            ok = r.returncode == 0 and os.path.exists(pre + "%d.txt" % j) and same(os.path.join(a.arch, "ref_%d.txt" % k), pre + "%d.txt" % j)
            n_ok += 1 if ok else 0
    fails += 0 if n_ok == len(prompts) else 1
    print("tl_host_m7, all %d prompts together, %d at a time: %d identical" % (len(prompts), B, n_ok), flush=True)
    print("ALL IDENTICAL" if not fails else "%d DIFFERENCES" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
