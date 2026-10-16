#!/usr/bin/env python3
"""suite.py -- run psram_llm tests on the Teensy unattended, three times each, against the PC reference.

    python firmware/bench-one/tests/psram_llm/suite.py [--flash] [--tests FILE] [--runs 3] [--bench 3]

THE PROCESS (docs: firmware/bench-one/tests/psram_llm/README.md, "Test process")
  1. --flash builds, archives and flashes through tools/bench_run.py (identity-guarded), then this script
     takes the port from the first boot line.
  2. The boot window is held with "::hold" so the default prompt does not run.
  3. "::bench" runs --bench times: the card, the kernels on real rows, PSRAM raw and self-checked.
  4. Every test (id | tokens to generate | prompt, \\n escapes) runs --runs times. Before its first run the
     PC reference (tests/tl_ref.exe, shared/model_q.c's fused path) is computed for the same prompt.
     A group between "@multi <id> | <tokens>" and "@end" (lines "id | prompt") is sent with ::multi and
     answered TOGETHER, one pass over the weights for all its prompts; each prompt is still compared
     with its own reference computed alone.
     Runs go in ROUNDS -- run 1 of every test, then run 2 of every test, ... -- with one ::bench at the
     start of each round, so a night that stops early has every test at least once.
  5. Each run is compared twice: with the PC reference (token ids exactly, logits numerically, first
     divergence and the reference's own margin there) and with this test's first run on the board (the
     board against itself must be bit-identical, or something on it is not deterministic).
  6. Everything lands in bench-archive/<stamp>-psram_llm-suite/: serial.log (every byte, host-timestamped
     at each line), <test>.expected.txt, results.jsonl (one line per run or bench), summary.md
     (regenerated after every run, so a night that stops early still has its table).

Real data only: every number in results.jsonl and summary.md is parsed from what the board printed."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
TESTS_DIR = os.path.abspath(os.path.join(HERE, ".."))
MODEL = "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba"
REF_EXE = "tl_ref.exe"   # --ref: the reference binary the board's firmware was proven against
STEP = re.compile(r'^step (\d+) pos (\d+) fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"$')
SSTEP = re.compile(r'^S(\d+) (step .*)$')
SPROMPT = re.compile(r'^S(\d+) (prompt ids:.*)$')
KV = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\S+)')


def now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


class Board:
    def __init__(self, outdir):
        self.log = open(os.path.join(outdir, "serial.log"), "ab")
        self.link = None
        self.buf = b""

    def note(self, s):
        self.log.write(("\n[suite %s] %s\n" % (now(), s)).encode())
        self.log.flush()
        print("[%s] %s" % (now(), s), flush=True)

    def port(self):
        from serial.tools import list_ports
        for p in list_ports.comports():
            if getattr(p, "vid", None) == 0x16C0:
                return p.device
        return None

    def ensure(self, wait_s=600):
        import serial
        t0 = time.time()
        while self.link is None:
            p = self.port()
            if p:
                try:
                    self.link = serial.Serial(p, 115200, timeout=0.5)
                    self.note("port %s open" % p)
                    return
                except Exception as e:
                    self.note("open %s failed: %s" % (p, e))
            if time.time() - t0 > wait_s:
                raise RuntimeError("no Teensy on USB for %d s" % wait_s)
            time.sleep(1)

    def send(self, line):
        self.ensure()
        self.link.write((line + "\n").encode("utf-8"))
        self.link.flush()
        self.log.write(("\n[suite %s] >>> %s\n" % (now(), line)).encode("utf-8", "replace"))
        self.log.flush()

    def lines(self, timeout):
        """Yield decoded lines as they arrive, until timeout seconds pass with nothing new."""
        last = time.time()
        while True:
            self.ensure()
            try:
                data = self.link.read(4096)
            except Exception as e:
                self.note("read failed (%s); reopening" % e)
                try:
                    self.link.close()
                except Exception:
                    pass
                self.link = None
                time.sleep(1)
                continue
            if data:
                last = time.time()
                self.buf += data
                while b"\n" in self.buf:
                    raw, self.buf = self.buf.split(b"\n", 1)
                    s = raw.decode("utf-8", "replace").rstrip("\r")
                    self.log.write(("%s | %s\n" % (time.strftime("%H:%M:%S"), s)).encode("utf-8", "replace"))
                    self.log.flush()
                    yield s
            elif time.time() - last > timeout:
                return

    def wait_for(self, pattern, timeout):
        rx = re.compile(pattern)
        got = []
        for s in self.lines(timeout):
            got.append(s)
            if rx.search(s):
                return s, got
        return None, got


def parse_steps(lines):
    steps, prompt = {}, None
    for s in lines:
        if s.startswith("prompt ids:"):
            prompt = s.split(":", 1)[1].split()
        m = STEP.match(s.strip())
        if m:
            steps[int(m.group(1))] = dict(fed=int(m.group(3)), t1=int(m.group(4)), l1=m.group(5),
                                          t2=int(m.group(6)), l2=m.group(7), text=m.group(8))
    return prompt, steps


def split_multi(lines):
    """A ::multi run's lines, split back into one line list per prompt with the "S<k> " prefix removed."""
    per = {}
    for s in lines:
        m = SSTEP.match(s.strip()) or SPROMPT.match(s.strip())
        if m:
            per.setdefault(int(m.group(1)), []).append(m.group(2))
    return per


def kv(line):
    return {k: (v[1:-1] if v.startswith('"') else v) for k, v in KV.findall(line)}


def unescape(s):
    """Exactly the firmware's unescape(): \\n \\t \\\\ and nothing else; any other backslash pair stays."""
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            out.append({"n": "\n", "t": "\t", "\\": "\\"}.get(c, "\\" + c))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def reference(test, outdir):
    path = os.path.join(outdir, "%s.expected.txt" % test["id"])
    if not os.path.exists(path):
        prompt = unescape(test["prompt"])
        exe = os.path.join(TESTS_DIR, REF_EXE)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            r = subprocess.run([exe, MODEL, prompt, str(test["gen"])], stdout=f, stderr=subprocess.DEVNULL,
                               cwd=TESTS_DIR)
        if r.returncode:
            raise RuntimeError("tl_ref failed for %s" % test["id"])
    with open(path, encoding="utf-8") as f:
        return parse_steps(f.read().splitlines())


def compare(ref, got):
    pe, E = ref
    pt, T = got
    out = dict(prompt_ids_match=(pe == pt), steps=len(T), matched=0, first_divergence=None,
               margin_at_divergence=None, max_logit_delta=0.0)
    for k in sorted(T):
        if k not in E:
            break
        e, t = E[k], T[k]
        if e["fed"] != t["fed"]:
            break
        if e["t1"] != t["t1"]:
            out["first_divergence"] = k
            out["margin_at_divergence"] = float(e["l1"]) - float(e["l2"])
            break
        out["matched"] += 1
        out["max_logit_delta"] = max(out["max_logit_delta"], abs(float(t["l1"]) - float(e["l1"])))
    return out


def same_as_first(first, got):
    """Board against itself: every step line identical, ids and every printed digit."""
    if first is None:
        return None
    return first[0] == got[0] and first[1] == got[1]


def load_tests(path):
    tests, group = [], None
    for raw in open(path, encoding="utf-8"):
        s = raw.rstrip("\n")
        if not s.strip() or s.lstrip().startswith("#"):
            continue
        if s.strip().startswith("@multi"):         # a group answered together
            gid, gen = [x.strip() for x in s.strip()[6:].split("|", 1)]
            group = dict(id=gid, gen=int(gen), multi=[])
            continue
        if s.strip() == "@end":
            if not group or not 1 <= len(group["multi"]) <= 8:
                raise SystemExit("@end without a group of 1..8 prompts")
            tests.append(group)
            group = None
            continue
        if group is not None:
            sid, prompt = [x.strip() if i < 1 else x.lstrip() for i, x in enumerate(s.split("|", 1))]
            group["multi"].append(dict(id="%s.%s" % (group["id"], sid), gen=group["gen"], prompt=prompt))
            continue
        if s.lstrip().startswith("::"):            # a board command, sent before the tests after it
            tests.append(dict(id=None, cmd=s.strip()))
            continue
        tid, gen, prompt = [x.strip() if i < 2 else x.lstrip() for i, x in enumerate(s.split("|", 2))]
        tests.append(dict(id=tid, gen=int(gen), prompt=prompt))
    if group is not None:
        raise SystemExit("@multi %s has no @end" % group["id"])
    return tests


def write_summary(outdir, results):
    fake = os.path.exists(os.path.join(outdir, "FAKE"))
    lines = ["# psram_llm suite -- %s" % os.path.basename(outdir), "",
             ("**DRY RUN against fake_board.py: step lines computed by tl_host.exe on the PC, every timing invented. "
              "Nothing here is a board measurement.**") if fake else
             "Every number below was printed by the board. Reference = shared/model_q.c on the PC, same prompt.", "",
             "## Runs", "",
             "| test | run | prompt tok | gen | passes | total s | s/pass | card MB/s | card s | compute s | hidden s | PSRAM s | max C | vs PC: same tokens | first divergence (ref margin) | max logit delta | same as run 1 | answer |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        if r.get("kind") != "run":
            continue
        c = r.get("vs_reference", {})
        div = "none" if c.get("first_divergence") is None else "step %s (%.4f)" % (c["first_divergence"], c["margin_at_divergence"])
        R = r.get("run_line", {})
        # hidden s (v7): arithmetic done while the card was reading. card s counts only the wait left over (so card
        # MB/s is bytes over that wait, not the card's own rate); compute s still includes the hidden part.
        lines.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s/%s | %s | %.3g | %s | %s |" % (
            r["test"], r["run"], R.get("n_prompt", "?"), R.get("n_gen", "?"), R.get("passes", "?"), R.get("total_s", "?"),
            R.get("per_pass_s", "?"), R.get("sd_mbs", "?"), R.get("sd_s", "?"), R.get("compute_s", "?"),
            R.get("hidden_s", "--"), R.get("ps_s", "?"),
            R.get("temp_max_c", "?"), c.get("matched", 0), c.get("steps", 0), div, c.get("max_logit_delta", 0.0),
            {True: "yes", False: "NO", None: "--"}[r.get("same_as_run1")], r.get("answer", "")[:60].replace("|", "/")))
    # The same prompt and token count reached by different paths (batched prompt, one token per pass, inside a
    # ::multi group) is the same arithmetic in the same order on the same board, so every step line -- ids and
    # every printed digit of every logit -- must be identical. steps_sha1 hashes exactly those lines.
    same = {}
    for r in results:
        if r.get("kind") == "run" and r.get("vs_reference", {}).get("steps"):
            same.setdefault((r["prompt"], r["gen"]), []).append(r)
    same = {k: v for k, v in same.items() if len({r["test"] for r in v}) > 1}
    if same:
        lines += ["", "## Same prompt, different paths (must be identical to the digit)", "",
                  "| prompt | gen | test | run | steps sha1 | same as the first |", "|---|---|---|---|---|---|"]
        for (p, gen), v in same.items():
            for r in v:
                lines.append("| `%s` | %d | %s | %d | %s | %s |" % (p[:40].replace("|", "/"), gen, r["test"], r["run"],
                                                                r["steps_sha1"][:12],
                                                                "yes" if r["steps_sha1"] == v[0]["steps_sha1"] else "NO"))
    g = [r for r in results if r.get("kind") == "multi"]
    if g:
        lines += ["", "## Prompts answered together (::multi)", "",
                  "| group | run | prompts | positions | passes | one at a time | s/pass | total s | compute s | card MB/s | max C | all = PC | same as run 1 |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in g:
            R = r.get("run_line", {})
            lines.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                r["test"], r["run"], R.get("multi", "?"), R.get("positions", "?"), R.get("passes", "?"),
                R.get("solo_passes", "?"), R.get("per_pass_s", "?"), R.get("total_s", "?"), R.get("compute_s", "?"),
                R.get("sd_mbs", "?"), R.get("temp_max_c", "?"), "%d/%d" % (r["all_match"], r["n"]),
                {True: "yes", False: "NO", None: "--"}[r.get("same_as_run1")]))
    b = [r for r in results if r.get("kind") == "bench"]
    if b:
        lines += ["", "## Benchmarks", "", "| run | line |", "|---|---|"]
        for r in b:
            for s in r["lines"]:
                lines.append("| %d | %s |" % (r["run"], s.replace("|", "/")))
    with open(os.path.join(outdir, "summary.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", action="store_true")
    ap.add_argument("--tests", default=os.path.join(HERE, "tests_night2.txt"))
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--bench", type=int, default=3)
    ap.add_argument("--note", default="")
    ap.add_argument("--wait-power-cycle", action="store_true",
                    help="wait for the Teensy to leave the USB bus and come back before starting")
    ap.add_argument("--wait-hours", type=float, default=16.0)
    ap.add_argument("--ref", default="tl_ref.exe",
                    help="reference binary in tests/ (tl_ref_v5.exe: the libm build firmware v5 was proven against)")
    ap.add_argument("--fake", action="store_true",
                    help="dry-run the harness against fake_board.py (real PC step lines, invented timings); needs --out")
    ap.add_argument("--out", default=None, help="output folder (required with --fake, which may not write to bench-archive)")
    a = ap.parse_args()

    global REF_EXE
    REF_EXE = a.ref
    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(ROOT, "bench-archive", "%s-psram_llm-suite" % stamp)
    if a.fake:
        # bench-archive holds what boards printed; a dry run's invented timings must never land there
        if not a.out or os.path.abspath(a.out).lower().startswith(os.path.join(ROOT, "bench-archive").lower()):
            sys.exit("--fake needs --out outside bench-archive")
        outdir = os.path.abspath(a.out)
        a.flash, a.wait_power_cycle = False, False
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, "FAKE"), "w") as ff:
            ff.write("fake_board.py dry run -- not a measurement\n")
    os.makedirs(outdir, exist_ok=True)
    tests = load_tests(a.tests)
    with open(os.path.join(outdir, "tests.txt"), "w", encoding="utf-8") as f:
        f.write(open(a.tests, encoding="utf-8").read())
    results = []
    resf = open(os.path.join(outdir, "results.jsonl"), "a", encoding="utf-8")

    def record(r):
        r["time"] = now()
        results.append(r)
        resf.write(json.dumps(r) + "\n")
        resf.flush()
        write_summary(outdir, results)

    if a.flash:
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "bench_run.py"), HERE, "3",
                            "suite flash: " + (a.note or os.path.basename(a.tests))], cwd=ROOT)
        if r.returncode:
            sys.exit("flash failed")

    if a.fake:
        from fake_board import FakeBoard
        B = FakeBoard(outdir)
    else:
        B = Board(outdir)
    B.note("suite start: %d entries x %d runs, bench x %d, tests file %s" % (len(tests), a.runs, a.bench, a.tests))
    if a.wait_power_cycle:
        # The SD card on a Teensy 4.1 is only reset by cutting its power; a hung card (SdFat 0x17) waits for this.
        B.note("waiting for the Teensy to be power-cycled: USB unplugged, then plugged back in")
        t0 = time.time()
        while B.port() and time.time() - t0 < a.wait_hours * 3600:
            time.sleep(1)
        B.note("Teensy left the bus")
        while not B.port() and time.time() - t0 < a.wait_hours * 3600:
            time.sleep(1)
        if not B.port():
            B.note("no power cycle within %.0f h" % a.wait_hours)
            sys.exit(4)
        B.note("Teensy back on the bus; it boots by itself")
        time.sleep(1.5)
    # Get to an idle board: the boot window (READY) or the end of a run (DONE), whichever comes.
    B.send("I")
    hit, _ = B.wait_for(r"READY\.|DONE\.|^I bench-one|HOLD|HALTED", 900)
    if hit and "HALTED" in hit:
        B.note("board halted during boot: %s" % hit)
        sys.exit(2)
    if hit and "READY" in hit:
        B.send("::hold")
        B.wait_for(r"HOLD", 30)
    elif hit is None:
        # a board mid-run prints steps; keep listening until it finishes
        hit, _ = B.wait_for(r"DONE\.|HALTED", 7200)
        if hit is None or "HALTED" in hit:
            B.note("board not usable: %s" % hit)
            sys.exit(2)
    B.send("I")
    ident, _ = B.wait_for(r"^I bench-one", 30)
    B.note("identity: %s" % ident)

    firsts = {}

    def run_one(t, run):
        ref = reference(t, outdir)
        B.send("::gen %d" % t["gen"])
        B.wait_for(r"^GEN ", 30)
        B.note("test %s run %d: gen %d prompt %r" % (t["id"], run, t["gen"], t["prompt"]))
        t0 = time.time()
        B.send(t["prompt"])
        est = (len(ref[0] or []) + t["gen"]) * 400 + 600
        hit, got = B.wait_for(r"^RUN |HALTED", est)
        B.wait_for(r"DONE\.", 30)
        steps = parse_steps(got)
        ans = next((s.split("ANSWER:", 1)[1].strip() for s in got if "ANSWER:" in s), "")
        first = firsts.get(t["id"])
        r = dict(kind="run", test=t["id"], run=run, gen=t["gen"], prompt=t["prompt"],
                 wall_s=round(time.time() - t0, 1), run_line=kv(hit) if hit and hit.startswith("RUN") else {},
                 raw_run_line=hit, vs_reference=compare(ref, steps), answer=ans,
                 same_as_run1=same_as_first(first, steps),
                 steps_sha1=hashlib.sha1(json.dumps(steps[1], sort_keys=True).encode()).hexdigest())
        if first is None:
            firsts[t["id"]] = steps
        record(r)
        B.note("test %s run %d done: %s" % (t["id"], run, json.dumps(r["vs_reference"])))
        return hit is not None and "HALTED" not in hit

    def run_group(t, run):
        refs = [reference(m, outdir) for m in t["multi"]]
        B.send("::gen %d" % t["gen"])
        B.wait_for(r"^GEN ", 30)
        B.send("::multi %d" % len(t["multi"]))
        B.wait_for(r"^MULTI ", 30)
        B.note("group %s run %d: %d prompts together, gen %d" % (t["id"], run, len(t["multi"]), t["gen"]))
        t0 = time.time()
        for m in t["multi"]:
            B.send(m["prompt"])
        hit, got = B.wait_for(r"^RUN |HALTED", 3600)
        B.wait_for(r"DONE\.", 30)
        per = split_multi(got)
        answers = {}
        for s in got:
            mm = re.match(r"^\s*ANSWER S(\d+): (.*)$", s)
            if mm:
                answers[int(mm.group(1))] = mm.group(2)
        n_match, same_all = 0, True
        for i, m in enumerate(t["multi"]):
            steps = parse_steps(per.get(i, []))
            first = firsts.get(m["id"])
            c = compare(refs[i], steps)
            full = c["prompt_ids_match"] and c["steps"] > 0 and c["matched"] == c["steps"]
            n_match += 1 if full else 0
            same = same_as_first(first, steps)
            if same is False:
                same_all = False
            if first is None:
                firsts[m["id"]] = steps
            record(dict(kind="run", test=m["id"], run=run, gen=m["gen"], prompt=m["prompt"], together=t["id"],
                        run_line=dict(n_prompt=str(len(steps[0] or [])), n_gen=str(m["gen"]), passes="(group)"),
                        raw_run_line=hit, vs_reference=c, answer=answers.get(i, ""),
                        same_as_run1=same,
                        steps_sha1=hashlib.sha1(json.dumps(steps[1], sort_keys=True).encode()).hexdigest()))
        g = dict(kind="multi", test=t["id"], run=run, n=len(t["multi"]), all_match=n_match,
                 same_as_run1=None if run == 1 else same_all, wall_s=round(time.time() - t0, 1),
                 run_line=kv(hit) if hit and hit.startswith("RUN") else {}, raw_run_line=hit)
        record(g)
        B.note("group %s run %d done: %d/%d prompts equal the PC reference; %s" % (t["id"], run, n_match, len(t["multi"]), hit))
        return hit is not None and "HALTED" not in hit

    for run in range(1, a.runs + 1):
        if run <= a.bench:
            B.send("::bench")
            _, got = B.wait_for(r"^BENCH end", 600)
            record(dict(kind="bench", run=run, lines=[s for s in got if s.startswith("BENCH")]))
        for t in tests:
            if t.get("cmd"):
                B.send(t["cmd"])
                ack, _ = B.wait_for(r"^[A-Z]+ ", 30)
                record(dict(kind="cmd", cmd=t["cmd"], reply=ack, run=run))
                B.note("board command %s -> %s" % (t["cmd"], ack))
                continue
            ok = run_group(t, run) if t.get("multi") else run_one(t, run)
            if not ok:
                B.note("stopping: the board did not finish the run")
                sys.exit(3)
        # what a pass costs by slot count, from every pass line so far (analyze_passes.py)
        with open(os.path.join(outdir, "passes.md"), "w", encoding="utf-8") as pf:
            subprocess.run([sys.executable, os.path.join(HERE, "analyze_passes.py"), os.path.join(outdir, "serial.log")],
                           stdout=pf, stderr=subprocess.STDOUT)
        B.note("round %d complete; passes.md updated" % run)
    for k in range(a.runs + 1, a.bench + 1):
        B.send("::bench")
        _, got = B.wait_for(r"^BENCH end", 600)
        record(dict(kind="bench", run=k, lines=[s for s in got if s.startswith("BENCH")]))
    B.note("suite complete")
    B.send("::stats")
    B.wait_for(r"^STATS ps_fix", 30)


if __name__ == "__main__":
    main()
