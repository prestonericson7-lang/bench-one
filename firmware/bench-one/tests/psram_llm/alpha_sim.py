#!/usr/bin/env python3
"""alpha_sim.py -- speculative decoding from the context, measured on the PC reference's own token sequences.

    python firmware/bench-one/tests/psram_llm/alpha_sim.py --cost <passcost serial.log> <survey dir> [<survey dir> ...]

docs/59 L1: a pass over k+1 positions can verify k guessed tokens at once; with greedy decoding the accepted
tokens are exactly the ones the model would have produced one at a time, so the answer cannot change. The
guesses here cost nothing: at each step, find the most recent earlier place in the context (prompt plus what
has been generated) where the last n tokens occurred, and guess the k tokens that followed them there.

Everything that decides the result is either a property of the model and the text (which tokens the model
picks, so whether a guess is right) or a board measurement (the time of a pass over P positions). The first
comes from tl_ref.exe's step lines -- the computation the board reproduces step for step (docs/54) -- and the
second from pass_cost.py's table, measured on the board. No PC timing is used anywhere.

For each prompt: S = prompt ids + the reference's greedy tokens. The prompt pass yields the first answer token
either way, so the context starts as prompt + that token. A pass feeds the last accepted token plus a draft
d0..d(k-1) (P = k+1 positions) and yields the model's tokens at those positions; d_i is accepted while it
equals the true token, and the model's own token at the first mismatch (or after the last draft) comes free,
so a pass yields accepted + 1 tokens. With no match the pass is the ordinary one-position pass. Exactly the
board's ::lookup (psram_llm_board.inc) and tests/tl_host.c TL_LOOKUP with TL_NMAX/TL_NMIN/TL_DRAFT."""
import argparse
import collections
import glob
import os
import re

STEP = re.compile(r"^step (\d+) pos \d+ fed (-?\d+) -> top1 (-?\d+) ")
EOS_IDS = (151643, 151645)          # Qwen2's <|endoftext|> and <|im_end|>: where tl_ref and the board stop


def load(survey_dir):
    """[(name, category, prompt_ids, generated_ids)] from a survey folder's NN.txt files and survey.jsonl"""
    import json
    cats = {}
    jl = os.path.join(survey_dir, "survey.jsonl")
    if os.path.exists(jl):
        for ln in open(jl, encoding="utf-8"):
            r = json.loads(ln)
            cats[r["n"]] = r.get("category", "?")
    out = []
    for f in sorted(glob.glob(os.path.join(survey_dir, "[0-9][0-9].txt"))):
        n = int(os.path.basename(f)[:2])
        prompt, steps = None, {}
        for ln in open(f, encoding="utf-8", errors="replace"):
            if ln.startswith("prompt ids:"):
                prompt = [int(x) for x in ln.split(":", 1)[1].split()]
                continue
            m = STEP.match(ln)
            if m:
                steps[int(m.group(1))] = int(m.group(3))
        if not prompt:
            continue
        gen = [steps[i] for i in range(len(prompt) - 1, max(steps) + 1)] if steps else []
        out.append(("%s/%02d" % (os.path.basename(survey_dir), n), cats.get(n, "?"), prompt, gen))
    return out


def lookup(ctx, nmax, nmin, k):
    """the k tokens that followed the most recent earlier occurrence of ctx's last n tokens (longest n first)"""
    L = len(ctx)
    for n in range(nmax, nmin - 1, -1):
        if L < n + 1:
            continue
        key = ctx[L - n:]
        for j in range(L - n - 1, -1, -1):         # most recent first; j+n <= L-1 so a follower exists
            if ctx[j:j + n] == key:
                return ctx[j + n:j + n + k]
    return []


def simulate(prompt, gen, nmax, nmin, k, budget=None):
    """[(positions, tokens yielded, guesses, accepted)] per pass for one answer, as the board's ::lookup runs it:
    the prompt pass has already produced gen[0], so the context starts as prompt + gen[0]; a pass never guesses
    past the last position the ::gen budget allows (the board's lim = n + gen - pos - 1) -- the budget, not where
    the answer happens to end, which the board cannot know. budget None: the answer's own length."""
    S = prompt + gen
    end = len(prompt) + budget + 1 if budget is not None else len(S)
    # an answer that ends with the end token: the board stops at the position that produced it, so a guess of
    # that token is never counted as kept (the tokens yielded are the same either way)
    last = len(S) - 1 if S and S[-1] in EOS_IDS else len(S)
    c, passes = len(prompt) + 1, []
    while c < len(S):
        d = lookup(S[:c], nmax, nmin, k)[:max(end - c - 1, 0)] if k else []
        acc = 0
        while acc < len(d) and c + acc < last and d[acc] == S[c + acc]:
            acc += 1
        y = min(acc + 1, len(S) - c)
        passes.append((len(d) + 1, y, len(d), acc))
        c += y
    return passes


def read_cost(path):
    """{positions: seconds} from a pass_cost.py serial.log: the board's own line for each pass. A pass over P
    positions mid-answer goes through the same batched path as a P-token prompt pass ("prefill=0..P-1: X s"); one
    position is the ordinary decode pass ("step=N: X s", the median of them). Also {P: temp_c} beside it."""
    cost, temps, dec = {}, {}, []
    last = None
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"    prefill=0\.\.(\d+): ([0-9.]+) s:", ln)
        if m:
            last = int(m.group(1)) + 1
            if last > 1:
                cost[last] = float(m.group(2))
            continue
        m = re.search(r"    step=\d+: ([0-9.]+) s:", ln)
        if m:
            dec.append(float(m.group(1))); last = 1
            continue
        m = re.search(r"STAGES .*temp_c=([0-9.]+)", ln)
        if m and last:
            temps.setdefault(last, float(m.group(1)))
    if dec:
        cost[1] = sorted(dec)[len(dec) // 2]
    return cost, temps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cost", required=True, action="append",
                    help="pass_cost.py's serial.log (measured on the board); repeat it to take each size's cheapest card path")
    ap.add_argument("--out", help="write the report here (markdown)")
    ap.add_argument("--switch-s", type=float, default=1.0,
                    help="seconds charged per FIFO<->ADMA2 card restart (board: under 1 s, host timestamps)")
    ap.add_argument("--admamin", type=int, default=3, help="the board's ::admamin (positions from which a pass uses ADMA2)")
    ap.add_argument("--budget", type=int, default=200, help="the ::gen budget the answers were generated under (survey.py: 200)")
    ap.add_argument("dirs", nargs="+")
    A = ap.parse_args()
    cost, temps = {}, {}
    for path in A.cost:                                  # each pass size at its cheapest measured card path
        c, t = read_cost(path)
        for P in c:
            if P not in cost or c[P] < cost[P]:
                cost[P], temps[P] = c[P], t.get(P, float("nan"))
    pmax = max(cost)
    items = [it for d in A.dirs for it in load(d)]
    rep = []
    w = rep.append
    w("# Speculative decoding from the context: what the reference's own answers say\n")
    w("%d answers, %d generated tokens, from %s. Pass costs measured on the board (`%s`): %s.\n" % (
        len(items), sum(len(g) for _, _, _, g in items), ", ".join(os.path.basename(d) for d in A.dirs),
        ", ".join(os.path.relpath(c).replace("\\", "/") for c in A.cost), ", ".join("%d pos %.1f s (%.1f C)" % (p, cost[p], temps.get(p, float("nan"))) for p in sorted(cost))))
    base = {}
    for name, cat, p, g in items:
        base[name] = max(len(g) - 1, 0) * cost[1]        # gen[0] comes from the prompt pass either way
    # sweep: k guesses (draft length), n = longest suffix tried, nmin = shortest
    results = []
    for nmax in (1, 2, 3, 4):
        for nmin in (1, 2):
            if nmin > nmax:
                continue
            for k in (1, 2, 3, 4, 5, 6, 7):
                if k + 1 > pmax:
                    continue
                tot_t = tot_b = 0.0
                prop = acc = toks = npass = 0
                bycat = collections.defaultdict(lambda: [0.0, 0.0, 0, 0])
                for name, cat, p, g in items:
                    ps = simulate(p, g, nmax, nmin, k, A.budget)
                    t = sum(cost[P] for P, *_ in ps)
                    # the board reads the card by FIFO below ::admamin positions and by ADMA2 from it; each change
                    # of path is a card restart (measured: under a second; charged A.switch_s each)
                    paths = [P >= A.admamin for P, *_ in ps]
                    t += A.switch_s * sum(1 for a, b in zip([False] + paths, paths) if a != b)
                    tot_t += t; tot_b += base[name]
                    prop += sum(x[2] for x in ps); acc += sum(x[3] for x in ps)
                    toks += sum(x[1] for x in ps); npass += len(ps)
                    bc = bycat[cat]; bc[0] += t; bc[1] += base[name]; bc[2] += sum(x[1] for x in ps); bc[3] += len(ps)
                results.append(dict(nmax=nmax, nmin=nmin, k=k, speed=tot_b / tot_t, alpha=acc / max(prop, 1),
                                    tpp=toks / npass, bycat={c: (v[1] / v[0], v[2] / v[3]) for c, v in bycat.items()}))
    results.sort(key=lambda r: -r["speed"])
    w("\n## Every setting, fastest first\n")
    w("speed-up = (tokens x one-position pass) / (sum of the passes actually needed); alpha = accepted guesses / guesses made; tokens/pass over all passes.\n")
    w("| longest n | shortest n | k guesses | speed-up | alpha | tokens/pass |\n|---|---|---|---|---|---|")
    for r in results:
        w("| %d | %d | %d | %.3fx | %.3f | %.2f |" % (r["nmax"], r["nmin"], r["k"], r["speed"], r["alpha"], r["tpp"]))
    best = results[0]
    cats = sorted(best["bycat"])
    w("\n## The fastest setting (n %d..%d, k %d), by kind of prompt\n" % (best["nmax"], best["nmin"], best["k"]))
    w("| kind | answers | speed-up | tokens/pass |\n|---|---|---|---|")
    cnt = collections.Counter(c for _, c, _, _ in items)
    for c in cats:
        sp, tpp = best["bycat"][c]
        w("| %s | %d | %.3fx | %.2f |" % (c, cnt[c], sp, tpp))
    worst = results[-1]
    w("\nSlowest setting: n %d..%d, k %d: %.3fx. A speed-up below 1 means the extra positions cost more than the guesses saved.\n"
      % (worst["nmax"], worst["nmin"], worst["k"], worst["speed"]))
    text = "\n".join(rep) + "\n"
    print(text)
    if A.out:
        open(A.out, "w", encoding="utf-8").write(text)


if __name__ == "__main__":
    main()
