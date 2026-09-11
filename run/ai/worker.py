#!/usr/bin/env python3
"""
worker.py -- run the local models as research grunts, for days, unattended

WHAT THIS IS FOR
----------------
Some work in this project is voluminous, tedious, and cheap to be wrong about. Reading a 2000-page
Zynq manual for six specific numbers. Staring at the same 200 lines of Verilog looking for the one
sign error. Enumerating every way a measurement could come out misleading. That work is worth doing
and it is a poor use of a metered assistant's time.

A local 30B coder model does it fine, for free, overnight, as many times as you like.

WHAT IT IS NOT FOR
------------------
Anything load-bearing. Every real finding in this project came from a measurement, and a model's
answer is a CANDIDATE, not a result. The tasks here are chosen so being wrong is cheap: they produce
suspicions to check, not conclusions to act on. Nothing here should ever be believed without being
verified against the code or the hardware.

That is why findings land in their own files rather than being written into the project.

HOW IT WORKS
------------
    ai/queue/*.task     work waiting. Drop a new file in and it gets picked up.
    ai/done/            tasks that completed, moved here so a restart resumes cleanly.
    ai/findings/        the answers, one file per task, timestamped.

A task file looks like:

    model: qwen2.5-coder:32b
    files: firmware/bench-one/fpga/rtl/gemv_int4.v
    ---
    the prompt goes here

NOT CRASHING THE PC
-------------------
One request at a time, never parallel. A sleep between tasks. A cap on generated tokens. A ceiling
on how much file content goes into a prompt. If Ollama fails repeatedly it stops rather than
hammering it. A wall-clock limit so an overnight run ends by itself.
"""

import json
import os
import shutil
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
QUEUE = os.path.join(HERE, "queue")
DONE = os.path.join(HERE, "done")
FINDINGS = os.path.join(HERE, "findings")
STATUS = os.path.join(HERE, "status.json")

OLLAMA = "http://localhost:11434"
# MODEL CHOICE IS DECIDED BY VRAM, NOT BY BENCHMARKS.
#
# This machine has an RTX 2080 Super with 8 GB. A 32B model is 20-22 GB, so 72% of it runs on the
# CPU and generation crawls at a token or two a second -- measured, the GPU sat at 99% waiting on
# the slow half. A 14B is 9 GB, so roughly three quarters stays resident and it runs many times
# faster. For candidate-generating review, several fast passes beat one slow one, because every
# answer gets verified anyway.
DEFAULT_MODEL = "qwen2.5-coder:14b"
NUM_CTX = 8192                        # see below -- this is not optional
MAX_FILE_CHARS = 20000                # must fit inside NUM_CTX with room for the answer
MAX_TOKENS = 2048
SLEEP_BETWEEN = 5                     # seconds, so the machine stays usable
MAX_HOURS = 12
MAX_CONSECUTIVE_FAILS = 3


def log(msg):
    stamp = time.strftime("%H:%M:%S")
    print("[%s] %s" % (stamp, msg), flush=True)


def status(**kw):
    """Heartbeat for the monitor window.

    Written after every state change rather than on a timer, so a stale timestamp means the worker
    is genuinely stuck rather than merely between updates. That distinction is the whole point of
    having a monitor: it should be able to tell "thinking hard" from "died".
    """
    try:
        kw["at"] = time.time()
        kw["at_human"] = time.strftime("%H:%M:%S")
        with open(STATUS, "w", encoding="utf-8") as f:
            json.dump(kw, f)
    except Exception:
        pass


def ollama_up():
    try:
        with urllib.request.urlopen(OLLAMA + "/api/tags", timeout=5) as r:
            return json.load(r)
    except Exception:
        return None


def ask(model, prompt, timeout=1800):
    body = json.dumps({
        "model": model,
        "prompt": prompt,
        "stream": False,
        # num_ctx MUST be set. Ollama defaults to 2048 tokens, which is about 8000 characters,
        # so a 10,000 character prompt silently loses its beginning -- exactly the part holding the
        # file being reviewed. The model then answers from the task text alone and produces
        # confident nonsense. Nothing warns you; the request succeeds.
        "options": {"num_predict": MAX_TOKENS, "num_ctx": NUM_CTX, "temperature": 0.2},
    }).encode()
    req = urllib.request.Request(OLLAMA + "/api/generate", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r).get("response", "")


def parse_task(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()
    head, _, prompt = raw.partition("\n---\n")
    meta = {}
    for line in head.splitlines():
        if ":" in line:
            k, _, v = line.partition(":")
            meta[k.strip().lower()] = v.strip()
    if not prompt.strip():
        prompt, meta = raw, {}
    return meta, prompt.strip()


def gather(meta):
    """Load the files a task names, truncating so one huge file cannot fill the context."""
    out = []
    spec = meta.get("files", "").strip()
    if not spec:
        return ""
    budget = MAX_FILE_CHARS
    for rel in [s.strip() for s in spec.split(",") if s.strip()]:
        p = os.path.join(ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(p):
            out.append("=== %s ===\n(NOT FOUND)\n" % rel)
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        if len(text) > budget:
            text = text[:budget] + "\n... (truncated)\n"
        budget -= len(text)
        out.append("=== %s ===\n%s\n" % (rel, text))
        if budget <= 0:
            break
    return "\n".join(out)


# WHAT THIS PREAMBLE LEARNED THE HARD WAY
#
# The first version asked for open-ended review: "find bugs, be specific, do not restate the code."
# A 14B model produced five confident non-findings, each restating what a line does and closing with
# "ensure that X". It matched the SHAPE of a review with none of the substance, and the failure was
# not the prompt wording -- it is that judgement is the thing these models are worst at.
#
# Generation is the thing they are good at. So every task now asks for an ARTEFACT that a script or
# a compiler can judge: test cases to run, mutations to apply, tables to diff against a reference.
# The model never decides whether something is wrong. It produces candidates and the toolchain
# decides. That needs no trust at all, which is the only footing worth building on here.
PREAMBLE = """You produce ARTEFACTS, not opinions. Something downstream will compile, run or diff
whatever you output, and that is what decides whether it was any good.

Rules:
- Output ONLY what is asked for, in the exact format asked for.
- No preamble, no explanation, no summary, no restating what the code does.
- Do not judge whether anything is correct. That is not your job here.
- If asked for N items, produce exactly N.
- If you cannot produce the artefact, output the single line: CANNOT
"""


def main():
    for d in (QUEUE, DONE, FINDINGS):
        os.makedirs(d, exist_ok=True)

    tags = ollama_up()
    if tags is None:
        status(state="no-ollama")
        log("Ollama is not answering on %s" % OLLAMA)
        log("Start it first: run D:\\start\\START.bat, or `ollama serve`")
        return 1
    have = sorted(m["name"] for m in tags.get("models", []))
    log("Ollama up, %d models" % len(have))

    started = time.time()
    fails = 0
    done_count = 0

    while True:
        if time.time() - started > MAX_HOURS * 3600:
            log("hit the %d hour limit, stopping" % MAX_HOURS)
            break
        tasks = sorted(f for f in os.listdir(QUEUE) if f.endswith(".task"))
        if not tasks:
            log("queue empty, %d done. Drop more .task files in %s" % (done_count, QUEUE))
            break

        name = tasks[0]
        path = os.path.join(QUEUE, name)
        meta, prompt = parse_task(path)
        model = meta.get("model", DEFAULT_MODEL)
        if model not in have:
            alt = next((m for m in have if m.split(":")[0] == model.split(":")[0]), None)
            log("%s not installed, using %s" % (model, alt or DEFAULT_MODEL))
            model = alt or DEFAULT_MODEL

        context = gather(meta)
        full = PREAMBLE + "\n" + (context + "\n" if context else "") + "TASK:\n" + prompt

        log("running %s with %s (%d chars in)" % (name, model, len(full)))
        status(state="working", task=name[:-5], model=model, chars=len(full),
               done=done_count, queued=len(tasks))
        t0 = time.time()
        try:
            answer = ask(model, full)
            fails = 0
        except Exception as e:
            fails += 1
            log("FAILED (%d/%d): %s" % (fails, MAX_CONSECUTIVE_FAILS, e))
            if fails >= MAX_CONSECUTIVE_FAILS:
                log("too many failures in a row, stopping so nothing gets hammered")
                return 1
            time.sleep(30)
            continue
        secs = time.time() - t0

        stamp = time.strftime("%Y%m%d-%H%M%S")
        out = os.path.join(FINDINGS, "%s-%s.md" % (name[:-5], stamp))
        with open(out, "w", encoding="utf-8") as f:
            f.write("# %s\n\n" % name[:-5])
            f.write("- model: %s\n- took: %.0f s\n- when: %s\n\n" % (model, secs, stamp))
            f.write("**This is a candidate, not a result. Verify before acting on it.**\n\n---\n\n")
            f.write(answer.strip() + "\n")
        log("  -> %s (%.0f s)" % (os.path.basename(out), secs))

        shutil.move(path, os.path.join(DONE, name))
        done_count += 1
        status(state="idle", task=name[:-5], model=model, secs=round(secs),
               done=done_count, queued=len(tasks) - 1, last_finding=os.path.basename(out))
        time.sleep(SLEEP_BETWEEN)

    status(state="finished", done=done_count, queued=0)
    log("finished, %d tasks done. Findings in %s" % (done_count, FINDINGS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
