#!/usr/bin/env python3
"""board_model_run.py -- prompts on the board, whatever model it has open, every printed step checked against tests/tl_ref.exe on
the same GGUF (on this PC), with the board's own timing. Written 2026-10-16 for the 30B mixture of experts; works for any model.

    python board_model_run.py --model <the same .gguf on this PC> [--gen 8] [--prompt TEXT ...] [--realistic]

Default prompts: the France chat question, then the realistic chat question (tests_realistic.txt chat_spi_i2c). Each prompt's
step lines are compared with tl_ref on the whole prompt (::reuse may skip positions the cache already holds; those print no
line). The RUN line's prompt_s / total_s and the per-pass card and compute times are reported. Die temperature is logged; the
run stops at PASSCOST_TEMP_STOP (default 75 C). Everything goes to bench-archive/<stamp>-psram_llm-model/."""
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
from suite import unescape  # noqa: E402
import chat_template  # noqa: E402

STEP = re.compile(r"^(?:R )?(step (\d+) pos \d+ fed .*)$")


def escape(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="the model file on this PC, the same bytes as on the card")
    ap.add_argument("--gen", type=int, default=8)
    ap.add_argument("--prompt", action="append", help="a raw prompt (chat template not applied); repeatable")
    ap.add_argument("--limit", type=float, default=14400, help="seconds to wait for one prompt")
    ap.add_argument("--pre", action="append", default=[], help="a board command sent before the prompts (repeatable), e.g. ::ringfault 1; "
                    "CMD@REGEX@SECONDS waits that long for a reply matching REGEX (::f16test runs for minutes: '::f16test@^F16TEST bad=@900')")
    A = ap.parse_args()
    if A.prompt:
        prompts = [("prompt %d" % i, p) for i, p in enumerate(A.prompt)]
    else:
        line = [x for x in open(os.path.join(HERE, "tests_realistic.txt"), encoding="utf-8") if x.startswith("chat_spi_i2c")][0]
        prompts = [("France", chat_template.render("What is the capital of France?")),
                   ("chat_spi_i2c", unescape(line.split("|", 2)[2].strip()))]
    out = os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-psram_llm-model")
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
            print(ln[:160], flush=True)
            tm = re.search(r"temp_c[= ]([0-9.]+)", ln)
            if tm and float(tm.group(1)) >= stop_at:
                send("::stop")
                sys.exit("die at %s C (limit %.0f): stopped" % (tm.group(1), stop_at))
            m = re.search(pat, ln)
            if m:
                return ln, m
        return None, None

    send("I")
    if rd_until(r"^I bench-one psram_llm", 120)[0] is None:
        sys.exit("the board did not answer I")
    ident = seen[-1]
    send("::gen %d" % A.gen); rd_until(r"^GEN", 10)
    for c in A.pre:
        cmd, pat, lim = (c.split("@") + [None, None])[:3] if "@" in c else (c, None, None)
        send(cmd)
        if rd_until(pat or r"^[A-Z]+ ", float(lim) if lim else 15)[0] is None and pat:
            sys.exit("no reply to %s matching %s in %s s" % (cmd, pat, lim))
    rows = []
    for name, prompt in prompts:
        n0 = len(seen)
        send(escape(prompt))
        ln, _ = rd_until(r"^RUN ", A.limit)
        part = seen[n0:]
        got = {int(m.group(2)): m.group(1) for m in (STEP.match(l) for l in part) if m}
        r = subprocess.run([os.path.join(TESTS, "tl_ref.exe"), A.model, prompt, str(A.gen)], capture_output=True, cwd=TESTS)
        ref = {int(m.group(2)): m.group(1) for m in (STEP.match(l) for l in r.stdout.decode("utf-8", "replace").splitlines()) if m}
        open(os.path.join(out, "ref_%s.txt" % name.replace(" ", "_")), "wb").write(r.stdout)
        same = bool(got) and all(ref.get(k) == v for k, v in got.items()) and max(got) == max(ref)
        kv = dict(x.split("=", 1) for x in ln.split()[1:] if "=" in x) if ln else {}
        ans = next((l.split("ANSWER:", 1)[1].strip() for l in part if "ANSWER:" in l), "")
        passes = [l for l in part if l.startswith("PASS ") or l.startswith("  pass ")]
        npr = int(kv.get("n_prompt", "0") or 0)
        after = max(got) - (npr - 1) if got and npr else 0          # answer tokens after the first (decode passes)
        dec = "%.1f" % ((float(kv["total_s"]) - float(kv["prompt_s"])) / after) if after > 0 and "total_s" in kv else "-"
        rows.append(dict(name=name, n=kv.get("n_prompt", "?"), steps=len(got), ref=len(ref), same=same,
                         prompt_s=kv.get("prompt_s", "?"), total_s=kv.get("total_s", "?"), dec=dec, mbs=kv.get("sd_mbs", "?"),
                         cmp=kv.get("compute_s", "?"), hid=kv.get("hidden_s", "?"),
                         tmax=kv.get("temp_max_c", "?"), answer=ans, run=ln or "no RUN line"))
        print("%s: %s (%d of %d step lines); %s" % (name, "IDENTICAL to tl_ref" if same else "DIFFERENT", len(got), len(ref), ln), flush=True)
    sm = ["# %s on the board, every printed step against tl_ref (::gen %d)\n" % (os.path.basename(A.model), A.gen), "`%s`\n" % ident,
          "| prompt | tokens | step lines vs tl_ref | first answer token s | each token after, s | card MB/s | compute s (hidden under reads) | die max C | answer |",
          "|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        sm.append("| %s | %s | %s (%d of %d) | %s | %s | %s | %s (%s) | %s | %s |" % (r["name"], r["n"], "IDENTICAL" if r["same"] else "**DIFFERENT**",
                  r["steps"], r["ref"], r["prompt_s"], r["dec"], r["mbs"], r["cmp"], r["hid"], r["tmax"], r["answer"].replace("|", "/")))
    sm.append("\n" + "\n".join("- `%s`" % r["run"] for r in rows))
    sm.append("\nAll printed step lines %s tl_ref." % ("match" if all(r["same"] for r in rows) else "do NOT all match"))
    text = "\n".join(sm) + "\n"
    open(os.path.join(out, "summary.md"), "w", encoding="utf-8").write(text)
    print(text)
    print("archive:", out)


if __name__ == "__main__":
    main()
