#!/usr/bin/env python3
"""pass_cost.py -- what one pass over P positions costs on the board, for P = 1..8.

    python firmware/bench-one/tests/psram_llm/pass_cost.py [P ...]

Speculative decoding verifies a draft of k tokens in ONE pass over k+1 positions, so its speed rests on the
cost of 2-, 3-, 4-position passes; only 1, 5 and 8 had ever been measured. A prompt of exactly P tokens with
::gen 1 is exactly one prefill pass over P positions (the board prints the token count, which is checked).
The board's own RUN line gives the time. Every line goes to bench-archive/<stamp>-psram_llm-passcost/serial.log
with a host timestamp; the table goes to passcost.md beside it."""
import os
import re
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
WORDS = "one two three four five six seven eight".split()


def main():
    ps = [int(x) for x in sys.argv[1:]] or list(range(1, 9))
    out = os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-psram_llm-passcost")
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "serial.log"), "a", encoding="utf-8")
    port = next(p.device for p in list_ports.comports() if getattr(p, "vid", None) == 0x16C0)
    s = serial.Serial(port, 115200, timeout=0.5)

    def send(cmd):
        log.write("%s > %s\n" % (time.strftime("%H:%M:%S"), cmd)); log.flush()
        s.write((cmd + "\n").encode())

    TEMP_STOP = float(os.environ.get("PASSCOST_TEMP_STOP", "75"))   # die temperature (C) at which to stop

    def rd_until(pat, limit):
        t0 = time.time()
        while time.time() - t0 < limit:
            line = s.readline().decode("utf-8", "replace").rstrip("\r\n")
            if line:
                log.write("%s | %s\n" % (time.strftime("%H:%M:%S"), line)); log.flush()
                tm = re.search(r"temp_c[= ]([0-9.]+)", line)
                if tm and float(tm.group(1)) >= TEMP_STOP:
                    send("::stop")
                    sys.exit("die at %s C (limit %.0f): stopped" % (tm.group(1), TEMP_STOP))
                m = re.search(pat, line)
                if m:
                    return line, m
        return None, None


    send("I")
    if rd_until(r"^I bench-one psram_llm", 60)[0] is None:
        sys.exit("the board did not answer I (busy, or not psram_llm)")
    send("::gen 1")
    rd_until(r"gen", 10)
    rows = []
    for P in ps:
        # a run of P prompt tokens: try the word list, and lengthen/shorten until the board counts P
        prompt = " ".join(WORDS[:P])
        send(prompt)
        line, m = rd_until(r'prompt: ".*"\s+\((\d+) tokens', 60)
        n = int(m.group(1)) if m else -1
        line, m = rd_until(r"^RUN .*", 900)
        if not line:
            sys.exit("no RUN line for P=%d" % P)
        kv = dict(x.split("=", 1) for x in line.split()[1:] if "=" in x)
        rows.append((P, n, kv))
        print("P=%d tokens=%d passes=%s total_s=%s sd_s=%s compute_s=%s hidden_s=%s" % (
            P, n, kv.get("passes"), kv.get("total_s"), kv.get("sd_s"), kv.get("compute_s"), kv.get("hidden_s")), flush=True)
    with open(os.path.join(out, "passcost.md"), "w", encoding="utf-8") as f:
        f.write("| positions (prompt tokens, as the board counted) | passes | total_s | sd_s | compute_s | hidden_s |\n|---|---|---|---|---|---|\n")
        for P, n, kv in rows:
            f.write("| %d | %s | %s | %s | %s | %s |\n" % (n, kv.get("passes"), kv.get("total_s"), kv.get("sd_s"),
                                                       kv.get("compute_s"), kv.get("hidden_s")))
    print("archive:", out)


if __name__ == "__main__":
    main()
