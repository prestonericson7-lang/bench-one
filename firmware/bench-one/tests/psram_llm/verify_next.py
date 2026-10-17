#!/usr/bin/env python3
"""verify_next.py -- every PC-side proof that the Teensy's core computes Qwen3-Next (the 80B hybrid) like the reference.

    python firmware/bench-one/tests/psram_llm/verify_next.py [--model PATH] [--gen 8] [--quick]

Correctness only (docs: pc-numbers-do-not-count). Needs tests/tl_ref.exe, tests/tl_host.exe and, if present,
tests/tl_host_f16c.exe (built with -mf16c) and tests/tl_host_m7.exe (-DGD_EMULATE_M7), all from the current tree.
Writes bench-archive/<stamp>-psram_llm-nextverify/{report.md, *.txt}; exits non-zero on any difference.

  1  FORMATS   the France prompt in each 16-bit state format (TL_STATE16 1 half, 2 bfloat16, 3 row-scaled int16):
               tl_host per token and prompt-batched against tl_ref in the same format, every step line
  2  PROMPTS   every verify_pc prompt (half, the default), per token and batched (skipped with --quick)
  3  FAULTS    a PSRAM bank dying mid-run (TL_PS_FAULT), and -- with the read-back off (TL_PSVERIFY=0) -- a write that
               silently lands wrong (TL_PS_CORRUPT): the lines printed, and the lines re-run after the recovery, equal
               the reference
  4  UNITS     the half rounding by F16C after its exhaustive self-test (tl_host_f16c.exe, TL_F16HW=1), and the
               Teensy's own SXTB16/SMLAD kernels emulated (tl_host_m7.exe), against the same reference
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, HERE)
from verify_pc import PROMPTS  # noqa: E402

FRANCE = "The capital of France is"


def steps(text):
    return [l for l in text.splitlines() if l.startswith("step ")]


def run(exe, model, prompt, gen, env=None):
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run([os.path.join(TESTS, exe), model, prompt, str(gen)], capture_output=True, cwd=TESTS, env=e)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default=os.environ.get("MODEL", "D:/start/models/Qwen3-Next-80B-A3B-Instruct-Q4_K_M.gguf"))
    ap.add_argument("--gen", type=int, default=8)
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    out = os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-psram_llm-nextverify")
    os.makedirs(out, exist_ok=True)
    tok = os.path.join(out, "next.tok")              # a persistent tokenizer store: no PSRAM writes at open
    rep, fails = [], 0

    def note(s):
        rep.append(s)
        print(s, flush=True)

    def same(name, ref, got_text):
        nonlocal fails
        got = steps(got_text)
        open(os.path.join(out, name + ".txt"), "w", encoding="utf-8").write(got_text)
        ok = got == ref and len(ref) > 0
        fails += 0 if ok else 1
        return ok

    note("# Qwen3-Next on the Teensy core -- PC verification, %s" % time.strftime("%Y-%m-%d %H:%M"))
    note("")
    note("model `%s`, %d generated tokens a prompt" % (a.model, a.gen))
    note("")
    note("## 1. The state formats")
    note("")
    ref_fr = {}
    for md in (1, 2, 3):
        rc, txt, _ = run("tl_ref.exe", a.model, FRANCE, a.gen, {"TL_STATE16": str(md)})
        open(os.path.join(out, "ref_france_m%d.txt" % md), "w", encoding="utf-8").write(txt)
        ref_fr[md] = steps(txt)
        for mode, env in (("per token", {}), ("prompt batched", {"TL_PREFILL": "1"})):
            env = dict(env, TL_STATE16=str(md), TL_TOKSTORE=tok)
            _, h, _ = run("tl_host.exe", a.model, FRANCE, a.gen, env)
            ok = same("host_france_m%d_%s" % (md, "b" if env.get("TL_PREFILL") else "t"), ref_fr[md], h)
            note("- state mode %d, %s: %s (%d lines)" % (md, mode, "IDENTICAL" if ok else "DIFFERENT", len(ref_fr[md])))
    note("")

    if not a.quick:
        note("## 2. Every prompt (half state)")
        note("")
        for k, p in enumerate(PROMPTS):
            _, txt, _ = run("tl_ref.exe", a.model, p, a.gen)
            ref = steps(txt)
            open(os.path.join(out, "ref_%d.txt" % k), "w", encoding="utf-8").write(txt)
            for mode, env in (("per token", {}), ("prompt batched", {"TL_PREFILL": "1"})):
                _, h, _ = run("tl_host.exe", a.model, p, a.gen, dict(env, TL_TOKSTORE=tok))
                ok = same("host_%d_%s" % (k, "b" if env else "t"), ref, h)
                note("- prompt %d, %s: %s (%d lines) -- %r" % (k, mode, "IDENTICAL" if ok else "DIFFERENT", len(ref), p[:50]))
        note("")

    note("## 3. Faults")
    note("")
    for name, env in (("a bank dies (bank 0, write 2500)", {"TL_PS_FAULT": "0:2500"}),
                      ("a write lands wrong unseen (read-back off; bank 0, unverified write 1500)",
                       {"TL_PSVERIFY": "0", "TL_PS_CORRUPT": "0:1500"})):
        rc, h, err = run("tl_host.exe", a.model, FRANCE, a.gen, dict(env, TL_PREFILL="1", TL_TOKSTORE=tok))
        recovered = "recovery:" in err and "identical" in err.split("recovery:")[-1].splitlines()[0] and rc == 0
        open(os.path.join(out, "fault_%s.err.txt" % list(env)[-1]), "w", encoding="utf-8").write(err)
        ok = same("fault_%s" % list(env)[-1], ref_fr[1], h) and recovered
        fails += 0 if recovered else 1
        note("- %s: %s" % (name, "recovered, every line IDENTICAL" if ok else "FAILED"))
    note("")

    note("## 4. Units")
    note("")
    if os.path.exists(os.path.join(TESTS, "tl_host_f16c.exe")):
        rc, h, err = run("tl_host_f16c.exe", a.model, FRANCE, a.gen, {"TL_F16HW": "1", "TL_PREFILL": "1", "TL_TOKSTORE": tok})
        st = [l for l in err.splitlines() if "self-test" in l]
        ok = same("host_f16c", ref_fr[1], h) and st and "unit is used" in st[0]
        fails += 0 if (st and "unit is used" in st[0]) else 1
        note("- F16C half rounding: %s; %s" % ("IDENTICAL" if ok else "DIFFERENT", st[0] if st else "no self-test line"))
    if os.path.exists(os.path.join(TESTS, "tl_host_m7.exe")):
        for mode, env in (("per token", {}), ("prompt batched", {"TL_PREFILL": "1"})):
            _, h, _ = run("tl_host_m7.exe", a.model, FRANCE, a.gen, dict(env, TL_TOKSTORE=tok))
            ok = same("m7_%s" % ("b" if env else "t"), ref_fr[1], h)
            note("- the M7 kernels, emulated, %s: %s" % (mode, "IDENTICAL" if ok else "DIFFERENT"))
    note("")
    note("**%s**" % ("EVERYTHING IDENTICAL" if not fails else "%d DIFFERENCES" % fails))
    open(os.path.join(out, "report.md"), "w", encoding="utf-8").write("\n".join(rep) + "\n")
    try:
        os.remove(tok)
    except OSError:
        pass
    print("archive:", out)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
