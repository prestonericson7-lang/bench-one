#!/usr/bin/env python3
"""ab_ffn.py -- the 2026-10-16 build on the board: the kernel cycle breakdown, then the batched pass's gate/up read
order A/B (::ffngroup 32, the old order, against 256), every answer checked.

    python firmware/bench-one/tests/psram_llm/ab_ffn.py

The reference is made here by tests/tl_ref.exe with the exact prompt passed as a process argument. Never
make it through a shell `$(python -c ...)`: on Windows Python's text-mode stdout turns LF into CR LF and the
substitution drops the final newline -- a different prompt (2026-10-16, corrected in that run's summary).

1. ::kbench -- the batched Q4_K kernel against the pre-widened one, cycles per weight per vector, rows in the
   row buffer and in OCRAM, 2..8 vectors (bit-identity checked on the board).
2. For G = 32 then 256: ::ffngroup G; a 2-token and a 5-token prompt with ::gen 1 (one P-position prompt pass,
   then one decode pass); then the realistic chat prompt (45 tokens, 4 generated) -- the first-answer-token time.
3. Every step line of the chat runs against the reference file (tl_ref.exe on the same prompt) and the two G
   against each other: they must be identical, or the run says so.
Die temperature is logged with every pass; the run stops if it reaches PASSCOST_TEMP_STOP (default 75 C).
Everything goes to bench-archive/<stamp>-psram_llm-abffn/{serial.log,summary.md}."""
import os
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, HERE)
from suite import MODEL, unescape  # noqa: E402  the firmware's exact unescape; the PC reference's model


def main():
    chat = [x for x in open(os.path.join(HERE, "tests_realistic.txt"), encoding="utf-8") if x.startswith("chat_spi_i2c")][0]
    chat_line = chat.split("|", 2)[2].strip()            # sent escaped, as suite.py sends it
    tests = os.path.abspath(os.path.join(HERE, ".."))
    r = subprocess.run([os.path.join(tests, "tl_ref.exe"), MODEL, unescape(chat_line), "4"], capture_output=True, cwd=tests)
    ref_steps = [l for l in r.stdout.decode("utf-8", "replace").splitlines() if l.startswith("step ")]
    out = os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-psram_llm-abffn")
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
            line = s.readline().decode("utf-8", "replace").rstrip("\r\n")
            if not line:
                continue
            log.write("%s | %s\n" % (time.strftime("%H:%M:%S"), line)); log.flush()
            seen.append(line)
            tm = re.search(r"temp_c[= ]([0-9.]+)", line)
            if tm and float(tm.group(1)) >= stop_at:
                send("::stop")
                sys.exit("die at %s C (limit %.0f): stopped" % (tm.group(1), stop_at))
            m = re.search(pat, line)
            if m:
                return line, m
        return None, None

    sm = []
    w = sm.append
    send("I")
    if rd_until(r"^I bench-one psram_llm", 60)[0] is None:
        sys.exit("the board did not answer I")
    w("# The 2026-10-16 build on the board\n")
    w("`%s`\n" % seen[-1])
    send("::kbench")
    rd_until(r"^KBENCH end", 600)
    w("\n## Kernel (::kbench): cycles per weight per vector, fewest of 5 runs over 16 rows of ffn_gate L0\n")
    w("| vectors | row buffer (DTCM), production kernel | row buffer, pre-widened | OCRAM, production | OCRAM, pre-widened | bit-identical | die C |")
    w("|---|---|---|---|---|---|---|")
    for l in seen:
        m = re.match(r"KBENCH np (\d+) rowbuf_n ([0-9.]+) rowbuf_nw ([0-9.]+) ocram_n ([0-9.]+) ocram_nw ([0-9.]+) .*identical (\S+) temp_c ([0-9.]+)", l)
        if m:
            w("| %s | %s | %s | %s | %s | %s | %s |" % m.groups())
    results = {}
    for G in (32, 256):
        send("::ffngroup %d" % G)
        rd_until(r"^FFNGROUP", 10)
        send("::gen 1")
        rd_until(r"^GEN", 10)
        for words in ("one two", "one two three four five"):
            n0 = len(seen)
            send(words)
            rd_until(r"^RUN ", 900)
            pl = [l for l in seen[n0:] if re.search(r"    prefill=0\.\.\d+: ", l)]
            tl = [l for l in seen[n0:] if l.startswith("STAGES prefill=")]
            m = re.search(r"prefill=0\.\.(\d+): ([0-9.]+) s: SD ([0-9.]+) MB in ([0-9.]+) s \(([0-9.]+) MB/s\).*compute ([0-9.]+) s", pl[0]) if pl else None
            t = re.search(r"temp_c=([0-9.]+)", tl[0]).group(1) if tl else "?"
            results[(G, len(words.split()))] = (m.groups() if m else None, t)
        send("::gen 4")
        rd_until(r"^GEN", 10)
        n0 = len(seen)
        send(chat_line)
        line, _ = rd_until(r"^RUN ", 3600)
        kv = dict(x.split("=", 1) for x in line.split()[1:] if "=" in x) if line else {}
        steps = [re.sub(r"^R ", "", l) for l in seen[n0:] if l.startswith("step ")]
        temps = [float(x) for x in re.findall(r"temp_c=([0-9.]+)", "\n".join(seen[n0:]))]
        results[(G, "chat")] = (kv, steps, temps)
    w("\n## The batched pass's gate/up read order: ::ffngroup 32 (the old order) against 256\n")
    w("| prompt pass | G | pass s | card MB | card wait s | card MB/s (wait only) | arithmetic s | die C |")
    w("|---|---|---|---|---|---|---|---|")
    for P in (2, 5):
        for G in (32, 256):
            g, t = results[(G, P)]
            if g:
                w("| %d positions | %d | %s | %s | %s | %s | %s | %s |" % (P, G, g[1], g[2], g[3], g[4], g[5], t))
    w("\n| realistic chat (45-token prompt, 4 generated) | G | total s | prompt s | per pass s | card wait s | hidden s | die C max |")
    w("|---|---|---|---|---|---|---|---|")
    for G in (32, 256):
        kv, steps, temps = results[(G, "chat")]
        w("| | %d | %s | %s | %s | %s | %s | %.1f |" % (G, kv.get("total_s"), kv.get("prompt_s"), kv.get("per_pass_s"),
                                                     kv.get("sd_s"), kv.get("hidden_s"), max(temps) if temps else float("nan")))
    a, b = results[(32, "chat")][1], results[(256, "chat")][1]
    w("\nStep lines: G=32 %d, G=256 %d, reference %d. G=32 vs G=256: %s. G=256 vs reference: %s." % (
        len(a), len(b), len(ref_steps), "IDENTICAL" if a == b else "DIFFERENT",
        "IDENTICAL" if b == ref_steps else "DIFFERENT"))
    text = "\n".join(sm) + "\n"
    open(os.path.join(out, "summary.md"), "w", encoding="utf-8").write(text)
    print(text)
    print("archive:", out)


if __name__ == "__main__":
    main()
