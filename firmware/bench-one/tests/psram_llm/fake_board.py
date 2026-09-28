#!/usr/bin/env python3
"""fake_board.py -- psram_llm v7's serial protocol, answered on the PC, for dry-running suite.py.

    python suite.py --fake --out <scratch dir> --runs 1

It is NOT a measurement and must never write into bench-archive (suite.py refuses): the step lines are real
(tests/tl_host.exe computes them, the same core the board runs), every timing and every BENCH/STATS number is
invented and printed as FAKE. What it proves is that the harness -- parsing, grouping, rounds, comparisons,
summary.md, passes.md -- does what it claims before a board spends hours on it."""
import os
import re
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
MODEL = "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba"
STEP = re.compile(r'^step (\d+) pos (\d+) fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"$')


def board_unescape(s):
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


def pass_line(what, k):
    sd, cmp_ = 97.1, 29.6 * k
    return ["    %s: %.1f s: SD 1833.9 MB in %.1f s (18.88 MB/s), PSRAM 1000 KB read 18.6 KB written in 0.78 s, "
            "compute %.1f s; PSRAM self-check so far: 0 reads corrected, 0 writes re-done, 0 unresolved   [FAKE]"
            % (what, sd + cmp_ + 0.78, sd, cmp_),
            "STAGES %s embed=0.001 qkv=%.2f att=0.030 wo=%.2f ffn=%.2f head=%.2f hidden=%.2f temp_c=50.0"
            % (what, 9.0 * k, 7.0 * k, 65.0 * k, 14.0 * k, 20.0 * k)]


class FakeBoard:
    def __init__(self, outdir):
        self.log = open(os.path.join(outdir, "serial.log"), "ab")
        self.q = []
        self.gen, self.prefill, self.want, self.multi, self.share = 16, 1, 0, [], 1
        self.dma, self.overlap = 1, 1
        self.out = outdir

    def note(self, s):
        self.log.write(("\n[suite %s] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), s)).encode())
        self.log.flush()
        print("[FAKE %s] %s" % (time.strftime("%H:%M:%S"), s), flush=True)

    def port(self):
        return "FAKE"

    def ensure(self, wait_s=0):
        pass

    def _say(self, lines):
        self.q.extend(lines)

    def _tl_host(self, args, env):
        e = dict(os.environ)
        e.update(env)
        r = subprocess.run([os.path.join(TESTS, "tl_host.exe"), MODEL] + args, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, cwd=TESTS, env=e)
        return r.stdout.decode("utf-8", "replace").replace("\r\n", "\n").splitlines()

    def send(self, line):
        self.log.write(("\n[suite %s] >>> %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), line)).encode("utf-8", "replace"))
        if line in ("I", "i"):
            return self._say(["I bench-one psram_llm v7 banks 7 ps_bytes 58720256 max_seq 2048 gen %d sdio %s overlap %d align 1 slice 0 FAKE"
                              % (self.gen, "dma" if self.dma else "fifo", self.overlap)])
        if line.startswith("::gen"):
            self.gen = max(1, min(512, int(line[5:] or 16)))
            return self._say(["GEN %d" % self.gen])
        if line.startswith("::prefill"):
            self.prefill = 1 if int(line[9:] or 0) else 0
            return self._say(["PREFILL %d" % self.prefill])
        if line == "::hold":
            return self._say(["HOLD waiting for prompts"])
        if line == "::stop":
            return self._say(["STOPPED (was idle)"])
        if line == "::bench":
            return self._say(["BENCH begin temp_c 50.0 FAKE", "BENCH sd_seq read_size 65536 mb_s 18.88 FAKE", "BENCH end temp_c 50.0"])
        if line == "::stats":
            return self._say(["STATS ps_fix_r 0 ps_fix_w 0 ps_bad 0 temp_c 50.0 gen %d max_seq 2048 uptime_s 1 FAKE" % self.gen])
        if line.startswith("::overlap"):
            self.overlap = 1 if int(line[9:] or 0) else 0
            return self._say(["OVERLAP %d (card %s)" % (self.overlap, "DMA_SDIO" if self.dma else "FIFO_SDIO: no overlap possible")])
        if line.startswith("::align"):
            return self._say(["ALIGN %d (card %s)" % (1 if int(line[7:] or 0) else 0, "DMA_SDIO" if self.dma else "FIFO_SDIO: reads stay exact")])
        if line.startswith("::slice"):
            v = max(0, int(line[7:] or 0))
            return self._say(["SLICE %d us of rows per yield%s" % (v, "" if v else " (0: one row per yield)")])
        if line.startswith("::sdio"):
            self.dma = 1 if "dma" in line else 0
            return self._say(["    qwen3b.gguf: 1929903072 bytes   [FAKE]", "    contiguous on the card   [FAKE]",
                              "SDIO %s" % ("dma" if self.dma else "fifo")])
        if line.startswith("::share"):
            self.share = 1 if int(line[7:] or 0) else 0
            return self._say(["SHARE %d" % self.share])
        if line.startswith("::multi"):
            self.want, self.multi = max(0, min(8, int(line[7:] or 0))), []
            return self._say(["MULTI %d" % self.want])
        if self.want > 0:
            self.multi.append(line)
            self.want -= 1
            if self.want == 0:
                self._run_multi()
            return
        self._run_prompt(board_unescape(line))

    def _run_prompt(self, prompt):
        out = self._tl_host([prompt, str(self.gen)], {"TL_PREFILL": "1"} if self.prefill else {})
        lines = ["", "  prompt: (FAKE board)"] + [s for s in out if s.startswith("prompt ids:")]
        steps = [s for s in out if STEP.match(s)]
        n = len(out[0].split(":", 1)[1].split()) if out and out[0].startswith("prompt ids:") else 0
        passes = 0
        if self.prefill:
            for p0 in range(0, n, 8):
                k = min(8, n - p0)
                lines += steps[p0:p0 + k] + pass_line("prefill=%d..%d" % (p0, p0 + k - 1), k)
                passes += 1
            rest = steps[n:]
        else:
            rest = steps
        base = n if self.prefill else 0
        for j, s in enumerate(rest):
            lines += [s] + pass_line("step=%d" % (base + j), 1)
            passes += 1
        ans = "".join(STEP.match(s).group(8) for s in steps if int(STEP.match(s).group(1)) >= n - 1)
        lines += ["", "  ANSWER: \"%s\"" % ans,
                  "RUN n_prompt=%d n_gen=%d prefill=%d passes=%d total_s=0.0 prompt_s=0.0 per_pass_s=%.2f sd_gb=0 sd_mbs=18.88 "
                  "sd_s=0 compute_s=0 hidden_s=0 ps_s=0 ps_fix_r=0 ps_fix_w=0 ps_bad=0 temp_max_c=50.0" % (n, self.gen, self.prefill, passes, 127.5),
                  "  DONE. Type another prompt and press Enter."]
        self._say(lines)

    def _run_multi(self):
        mfile = os.path.join(self.out, "_fake_multi.txt")
        with open(mfile, "w", encoding="utf-8", newline="\n") as f:
            for p in self.multi:
                f.write(p + "\n")                 # tl_host TL_MULTI unescapes exactly as the board does
        pref = os.path.join(self.out, "_fake_multi_")
        e = dict(os.environ)
        e.update({"TL_MULTI": mfile, "TL_MULTI_OUT": pref, "TL_MULTI_SHARE": str(self.share)})
        r = subprocess.run([os.path.join(TESTS, "tl_host.exe"), MODEL, "-", str(self.gen)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, cwd=TESTS, env=e)
        lines = []
        per = []
        for i in range(len(self.multi)):
            L = open(pref + "%d.txt" % i, encoding="utf-8").read().replace("\r\n", "\n").splitlines()
            per.append(L)
            lines.append("S%d %s" % (i, L[0]))
        # interleave the sequences' step lines the way passes would, one line each in turn
        idx = [1] * len(per)
        while any(idx[i] < len(per[i]) for i in range(len(per))):
            for i in range(len(per)):
                if idx[i] < len(per[i]):
                    lines.append("S%d %s" % (i, per[i][idx[i]]))
                    idx[i] += 1
        summ = next((s for s in r.stderr.decode("utf-8", "replace").splitlines() if s.startswith("multi:")), "")
        m = re.search(r"(\d+) passes over the weights for (\d+) positions .*: (\d+) passes\)", summ)
        passes, pos, solo = (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else (0, 0, 0)
        for p in range(passes):
            lines += pass_line("multi pass=%d slots=%d" % (p + 1, 8 if p < passes // 2 else 3), 8 if p < passes // 2 else 3)
        for i, L in enumerate(per):
            n = len(L[0].split(":", 1)[1].split())
            ans = "".join(STEP.match(s).group(8) for s in L[1:] if STEP.match(s) and int(STEP.match(s).group(1)) >= n - 1)
            lines.append("  ANSWER S%d: \"%s\"" % (i, ans))
        lines += ["RUN multi=%d n_gen=%d passes=%d positions=%d solo_passes=%d total_s=0.0 per_pass_s=0 sd_gb=0 sd_mbs=18.88 "
                  "sd_s=0 compute_s=0 hidden_s=0 ps_s=0 ps_fix_r=0 ps_fix_w=0 ps_bad=0 temp_max_c=50.0" % (len(per), self.gen, passes, pos, solo),
                  "  DONE. Type another prompt and press Enter."]
        self._say(lines)

    def lines(self, timeout):
        while self.q:
            s = self.q.pop(0)
            self.log.write(("%s | %s\n" % (time.strftime("%H:%M:%S"), s)).encode("utf-8", "replace"))
            self.log.flush()
            yield s

    def wait_for(self, pattern, timeout):
        rx = re.compile(pattern)
        got = []
        for s in self.lines(timeout):
            got.append(s)
            if rx.search(s):
                return s, got
        return None, got
