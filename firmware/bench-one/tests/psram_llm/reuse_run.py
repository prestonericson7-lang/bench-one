#!/usr/bin/env python3
"""reuse_run.py -- the attention cache reused across prompts (::reuse 1) on the board, every answer checked.

    python firmware/bench-one/tests/psram_llm/reuse_run.py [--gen 4]

Three chat prompts in a row on one boot:
  1. the realistic chat question (tests_realistic.txt chat_spi_i2c) -- the cache starts empty;
  2. a different question with the same system turn -- the board should feed it from position 24;
  3. the second turn of conversation 1: its prompt, the board's own answer to it, then a new user turn -- the
     board should feed it from where the conversation so far ends.
Each prompt's printed step lines are compared with tests/tl_ref.exe on that whole prompt (the board prints none
for the positions it reused); the REUSE line, the prompt time and the first-answer-token time are reported.
Die temperature is logged; the run stops at PASSCOST_TEMP_STOP (default 75 C).
Everything goes to bench-archive/<stamp>-psram_llm-reuse/{serial.log,summary.md}."""
import argparse
import os
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
from suite import MODEL, unescape  # noqa: E402
import chat_template  # noqa: E402

STEP = re.compile(r"^(?:R )?(step (\d+) pos \d+ fed .*)$")


def escape(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def tl_ref(prompt, gen):
    r = subprocess.run([os.path.join(TESTS, "tl_ref.exe"), MODEL, prompt, str(gen)], capture_output=True, cwd=TESTS)
    return {int(m.group(2)): m.group(1) for m in (STEP.match(l) for l in r.stdout.decode("utf-8", "replace").splitlines()) if m}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", type=int, default=4)
    ap.add_argument("--conversation", action="store_true", help="one conversation's turns back to back")
    A = ap.parse_args()
    line = [x for x in open(os.path.join(HERE, "tests_realistic.txt"), encoding="utf-8") if x.startswith("chat_spi_i2c")][0]
    p1 = unescape(line.split("|", 2)[2].strip())
    p2 = chat_template.render("What is the difference between UART and SPI? Answer in one sentence.")
    out = os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-psram_llm-reuse")
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "serial.log"), "a", encoding="utf-8")
    port = next(p.device for p in list_ports.comports() if getattr(p, "vid", None) == 0x16C0)
    s = serial.Serial(port, 115200, timeout=0.5)
    stop_at = float(os.environ.get("PASSCOST_TEMP_STOP", "75"))
    seen = []

    def send(cmd):
        log.write("%s > %s\n" % (time.strftime("%H:%M:%S"), cmd)); log.flush()
        s.write((cmd + "\n").encode())

    def rd_until(pat, limit):
        t0 = time.time()
        while time.time() - t0 < limit:
            ln = s.readline().decode("utf-8", "replace").rstrip("\r\n")
            if not ln:
                continue
            log.write("%s | %s\n" % (time.strftime("%H:%M:%S"), ln)); log.flush()
            seen.append(ln)
            tm = re.search(r"temp_c[= ]([0-9.]+)", ln)
            if tm and float(tm.group(1)) >= stop_at:
                send("::stop")
                sys.exit("die at %s C (limit %.0f): stopped" % (tm.group(1), stop_at))
            m = re.search(pat, ln)
            if m:
                return ln, m
        return None, None

    send("I")
    if rd_until(r"^I bench-one psram_llm", 60)[0] is None:
        sys.exit("the board did not answer I")
    ident = seen[-1]
    send("::gen %d" % A.gen); rd_until(r"^GEN", 10)
    send("::reuse 1"); rd_until(r"^REUSE \d", 10)
    rows = []

    def run(name, prompt):
        n0 = len(seen)
        send(escape(prompt))
        ln, _ = rd_until(r"^RUN ", 7200)
        part = seen[n0:]
        got = {int(m.group(2)): m.group(1) for m in (STEP.match(l) for l in part) if m}
        ref = tl_ref(prompt, A.gen)
        same = bool(got) and all(ref.get(k) == v for k, v in got.items()) and max(got) == max(ref)
        reuse = next((int(re.search(r"m=(\d+)", l).group(1)) for l in part if l.startswith("REUSE m=")), 0)
        kv = dict(x.split("=", 1) for x in ln.split()[1:] if "=" in x) if ln else {}
        ans = next((l.split("ANSWER:", 1)[1].strip() for l in part if "ANSWER:" in l), "")
        first = None
        # the first answer token comes with the last prompt pass: the prompt time
        rows.append(dict(name=name, n=int(kv.get("n_prompt", 0)), reuse=reuse, steps=len(got), ref=len(ref), same=same,
                         prompt_s=float(kv.get("prompt_s", 0)), total_s=float(kv.get("total_s", 0)), answer=ans,
                         tmax=float(kv.get("temp_max_c", "nan"))))
        r = rows[-1]
        print("%s: %d prompt tokens, reused %d, %d step lines printed %s, prompt %.1f s, total %.1f s, answer %s" % (
            name, r["n"], reuse, r["steps"], "IDENTICAL to tl_ref" if same else "DIFFERENT", r["prompt_s"], r["total_s"], ans), flush=True)
        return ans

    def follow(prev_prompt, answer, question):
        """the next turn: what came before, the board's own answer (unescaped), a new user turn"""
        text = answer.strip('"').encode("utf-8").decode("unicode_escape") if answer else ""
        return prev_prompt + text + "<|im_end|>\n<|im_start|>user\n" + question + "<|im_end|>\n<|im_start|>assistant\n"

    if A.conversation:
        # one conversation, its turns back to back -- each prompt holds everything before it
        a1 = run("turn 1", p1)
        t2 = follow(p1, a1, "And which one needs fewer wires?")
        a2 = run("turn 2", t2)
        t3 = follow(t2, a2, "Which one is faster?")
        run("turn 3", t3)
    else:
        a1 = run("1 the question, cold cache", p1)
        run("2 another question, same system turn", p2)
        run("3 conversation 1, turn 2", follow(p1, a1, "And which one needs fewer wires?"))
    sm = ["# The attention cache reused across prompts (::reuse 1, ::gen %d)\n" % A.gen, "`%s`\n" % ident,
          "| prompt | tokens | positions reused | step lines printed vs tl_ref | prompt s (to the first answer token) | total s | die max C |",
          "|---|---|---|---|---|---|---|"]
    for r in rows:
        sm.append("| %s | %d | %d | %s (%d of %d) | %.1f | %.1f | %.1f |" % (r["name"], r["n"], r["reuse"],
                  "IDENTICAL" if r["same"] else "**DIFFERENT**", r["steps"], r["ref"], r["prompt_s"], r["total_s"], r["tmax"]))
    sm.append("\nAll printed step lines %s tl_ref on the whole prompt." % ("match" if all(r["same"] for r in rows) else "do NOT all match"))
    text = "\n".join(sm) + "\n"
    open(os.path.join(out, "summary.md"), "w", encoding="utf-8").write(text)
    print(text)
    print("archive:", out)


if __name__ == "__main__":
    main()
