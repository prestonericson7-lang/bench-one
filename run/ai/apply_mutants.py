#!/usr/bin/env python3
"""
apply_mutants.py -- let the testbench judge the model's work

THE LOOP THIS CLOSES
--------------------
A local model is poor at judgement and decent at generation. Asked to review code it produces
confident non-findings; asked to produce twenty small edits that change behaviour it does a
perfectly good job, because that is generation.

So the model writes mutations, this applies them one at a time, and the EXISTING TESTBENCH decides
whether each one is caught. Nothing the model says is believed. The compiler and the simulator do
all the judging.

WHAT THE OUTPUT MEANS
---------------------
    CAUGHT      the testbench failed, as it should. Good, no action.
    SURVIVED    the behaviour changed and every test still passed. THIS IS THE PRIZE. It means the
                test suite has a hole, and the hole is real regardless of whether the mutation
                itself was a sensible edit.
    EQUIVALENT  the mutation compiled but provably cannot change behaviour. Also worth knowing:
                three of these turned up by hand today and each one marked a comment that claimed
                more than the code delivered.
    NOAPPLY     OLD text not found, or found more than once. The model's fault, and free to discard.
    BROKEN      would not compile. Discarded; a mutant has to be a valid program to mean anything.

A survivor is not automatically a bug in the design. It is always a gap in the tests, which is the
thing worth fixing either way.

RUN
    python apply_mutants.py <findings.md> <target-file> <build-and-run-command>

  e.g.
    python apply_mutants.py findings/01-mutants-gemv-*.md \\
        ../../firmware/bench-one/fpga/rtl/gemv_int4.v \\
        "iverilog -g2005 -o _m.vvp {MUT} tb_gemv_int4.v && vvp _m.vvp"

  {MUT} in the command is replaced by the path to the mutated copy.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def parse_mutations(path, source):
    """Pull OLD/NEW pairs out of whatever the model wrapped them in.

    The separator is NOT trusted. Asked for a tab, a model will cheerfully use spaces, a pipe, an
    arrow, or nothing consistent at all, and rejecting those throws away work that is otherwise
    perfectly good -- one run produced twenty usable mutations and parsed as zero.

    So instead of assuming a delimiter, every plausible split point on the line is tried and the one
    kept is whichever leaves a left-hand side that appears in the source file EXACTLY ONCE. That is
    the property the mutation needs anyway, so validating it doubles as parsing it, and the model's
    formatting stops mattering entirely.
    """
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip()
            if not line.strip() or line.startswith("#") or line.startswith("```"):
                continue
            if line.strip().upper().startswith("OLD"):      # a header row
                continue
            line = re.sub(r"^\s*\d+[.)]\s*", "", line)

            best = None
            # Longest valid OLD wins: a shorter prefix may also appear once but is likelier to be a
            # coincidence, and the model meant the longest thing it typed on the left.
            for cut in range(len(line) - 1, 0, -1):
                old = line[:cut].strip()
                new = line[cut:].strip()
                if len(old) < 4 or not new or old == new:
                    continue
                if source.count(old) == 1:
                    best = (old, new)
                    break
            if best:
                out.append(best)
    return out

def run(cmd, cwd, timeout=300):
    try:
        p = subprocess.run(cmd, shell=True, cwd=cwd, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -9, "(timed out)"


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    findings, target, cmd = sys.argv[1], sys.argv[2], sys.argv[3]
    workdir = os.path.dirname(os.path.abspath(sys.argv[4])) if len(sys.argv) > 4 else os.getcwd()

    with open(target, "r", encoding="utf-8") as f:
        original = f.read()

    muts = parse_mutations(findings, original)
    if not muts:
        print("no OLD<TAB>NEW pairs found in %s" % findings)
        return 1
    print("\n  %d mutations from %s" % (len(muts), os.path.basename(findings)))
    print("  target: %s" % target)
    print("  judge:  %s\n" % cmd)

    # Baseline: the unmutated code must pass, or every result below is meaningless.
    tmp = target + ".mut"
    shutil.copyfile(target, tmp)
    rc, _ = run(cmd.replace("{MUT}", tmp), workdir)
    if rc != 0:
        print("  BASELINE FAILS. Fix the tests before mutating anything.")
        os.remove(tmp)
        return 1
    print("  baseline passes\n")

    tally = {"CAUGHT": 0, "SURVIVED": 0, "NOAPPLY": 0, "BROKEN": 0}
    survivors = []

    for i, (old, new) in enumerate(muts, 1):
        n = original.count(old)
        if n != 1:
            tally["NOAPPLY"] += 1
            print("  %2d NOAPPLY   (appears %d times) %s" % (i, n, old[:52]))
            continue
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(original.replace(old, new, 1))

        rc, out = run(cmd.replace("{MUT}", tmp), workdir)
        if rc != 0 and ("error" in out.lower() and "FAIL" not in out):
            tally["BROKEN"] += 1
            print("  %2d BROKEN    %s" % (i, old[:52]))
        elif rc != 0 or "FAIL" in out:
            tally["CAUGHT"] += 1
            print("  %2d caught    %s" % (i, old[:52]))
        else:
            tally["SURVIVED"] += 1
            survivors.append((old, new))
            print("  %2d SURVIVED  %s  ->  %s" % (i, old[:40], new[:40]))

    os.remove(tmp)

    print("\n  caught %d, SURVIVED %d, broken %d, did not apply %d"
          % (tally["CAUGHT"], tally["SURVIVED"], tally["BROKEN"], tally["NOAPPLY"]))
    if survivors:
        print("\n  SURVIVORS -- each is a hole in the test suite:")
        for old, new in survivors:
            print("    %s\n      -> %s" % (old, new))
        print("\n  A survivor may be an equivalent mutation rather than a missed bug, but the")
        print("  test suite cannot tell those apart, and neither can you until you look.")
    else:
        print("\n  no survivors. The suite caught every behaviour change the model could invent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
