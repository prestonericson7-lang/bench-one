#!/usr/bin/env python3
"""verify_pc.py -- every PC-side proof that the Teensy's core computes the reference model, in one run.

    python firmware/bench-one/tests/psram_llm/verify_pc.py [--quick] [--no-llama]

Correctness only (docs: pc-numbers-do-not-count). Needs tests/tl_ref.exe and tests/tl_host.exe built from the
current tree (build lines in tests/tl_ref.c and tests/tl_host.c). Writes
bench-archive/<stamp>-psram_llm-pcverify/{report.md, *.txt} and exits non-zero on any difference.

  1  TOKENIZER  tl_core.c's PSRAM tokenizer against shared/tokenizer.c, id for id and with decode(encode(s))
                == s, on (a) every line of this repository's docs and shared sources and (b) 20,000 random
                records (fixed seed) mixing ASCII, other scripts, Unicode digits and spaces, emoji, special
                tokens and near-misses, contractions, runs of up to 400 characters, CR/LF combinations.
                If llama.cpp's server is up on 127.0.0.1:8089 (Ollama ships llama-server.exe), both sets are
                also compared with llama.cpp tokenizing the same file -- the independent reference.
  2  FORWARD    for each prompt, tl_host (weights from the file every token, PSRAM a separate store) against
                tl_ref (shared/model_q.c's fused path): every step line -- ids, both logits to nine digits,
                second-best token -- per token, and again with the prompt batched (TL_PREFILL=1).
  3  TOGETHER   the same prompts answered together (TL_MULTI, tl_multi_pass), shared openings off and on:
                each prompt's lines against its own tl_ref run.
  4  M7         if tests/dot_verify_m7.exe and tests/tl_host_m7.exe exist (README: built with -DGD_EMULATE_M7),
                the Teensy's own SXTB16/SMLAD kernels run on this PC: the kernel check, and whole forward
                passes -- per token, batched, all prompts together -- against the same references.
"""
import argparse
import glob
import json
import os
import random
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.abspath(os.path.join(HERE, ".."))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
MODEL = os.environ.get("MODEL", "D:/start/ollama-models/blobs/sha256-4a188102020e9c9530b687fd6400f775c45e90a0d7baafe65bd0a36963fbb7ba")  # MODEL=... for another GGUF
sys.path.insert(0, HERE)
import chat_template  # noqa: E402

PROMPTS = [
    "The capital of France is",
    "def is_prime(n):\n",
    "Q: What is 17 + 25?\nA:",
    "const add = (a, b) =>",
    chat_template.render("Name three primary colors."),
    chat_template.render('Translate "good morning" into French.'),
    chat_template.render("What does PSRAM stand for?"),
    chat_template.render("Summarize in one sentence: the Teensy 4.1 has a 600 MHz Cortex-M7 -- and pads for PSRAM.\n\n  Done."),
]

PALETTE = {
    "ascii_word": (30, lambda r: "".join(r.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(r.randint(1, 9)))),
    "digits": (8, lambda r: "".join(r.choice("0123456789") for _ in range(r.randint(1, 7)))),
    "punct": (12, lambda r: "".join(r.choice("!\"#$%&()*+,-./:;<=>?@[\\]^_`{|}~'") for _ in range(r.randint(1, 5)))),
    "space": (12, lambda r: r.choice([" ", "  ", "   ", "\t", " \t", "\t\t"])),
    "newline": (8, lambda r: r.choice(["\n", "\n\n", "\r\n", "\r", " \n", "\n ", "  \n\n  ", "\r\n\r\n", "\n\t", "\x0b", "\x0c"])),
    "contraction": (5, lambda r: r.choice(["'s", "'S", "'t", "'T", "'re", "'RE", "'Re", "'ve", "'VE", "'m", "'M", "'ll", "'LL",
                                           "'Ll", "'d", "'D", "'x", "'", "''", "'sam", "'rel"])),
    "uni_letter": (6, lambda r: r.choice(["é", "ß", "Ж", "дом", "α", "Ωμέγα", "中", "文字", "日本語", "한국", "مرحبا", "ब", "हिन्दी",
                                          "ñ", "Ł", "ǅ", "ʰ", "ı", "ﬁ", "ｱ", "ｶﾀｶﾅ"])),
    "uni_number": (3, lambda r: r.choice(["٣", "٤٥", "²", "½", "Ⅻ", "①", "੩", "३", "〇", "𝟙"])),
    "uni_space": (3, lambda r: r.choice(["\u00a0", "\u2003", "\u3000", "\u2028", "\u0085", "\u2009", "\u202f", "\u205f", "\u1680"])),
    "uni_other": (5, lambda r: r.choice(["—", "–", "\u201c", "\u201d", "…", "←", "↔", "€", "©", "😀", "👍🏽", "\u200d", "\ufeff",
                                         "\u0301", "e\u0301", "·", "§", "°", "™", "✓", "│", "┌─┐"])),
    "special": (4, lambda r: r.choice(["<|im_start|>", "<|im_end|>", "<tool_call>", "</tool_call>", "<|endoftext|>",
                                       "<|fim_prefix|>", "<|file_sep|>", "<|notatoken|>", "<|im_end", "<s>", "<|", "|>",
                                       "<>", "<|im_start|>user\n", "<|im_end|>\n"])),
    "long_run": (1, lambda r: r.choice("=-_*#~. \n") * r.randint(20, 400)),
}


def run(exe, args, env_extra, out_path):
    env = dict(os.environ)
    env.update(env_extra)
    with open(out_path, "wb") as f:
        r = subprocess.run([os.path.join(TESTS, exe), MODEL] + args, stdout=f, stderr=subprocess.PIPE, cwd=TESTS, env=env)
    return r.returncode, r.stderr.decode("utf-8", "replace")


def same(a, b):
    ra = open(a, "rb").read().replace(b"\r\n", b"\n")
    rb = open(b, "rb").read().replace(b"\r\n", b"\n")
    return ra == rb


def llama_up():
    try:
        return b"ok" in urllib.request.urlopen("http://127.0.0.1:8089/health", timeout=2).read()
    except Exception:
        return False


def llama_tok(text):
    req = urllib.request.Request("http://127.0.0.1:8089/tokenize", data=json.dumps({"content": text}).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=60).read())["tokens"]


def dump_ids(path):
    ids = {}
    for s in open(path, encoding="utf-8"):
        if s.startswith("L"):
            head, rest = s.split(":", 1)
            ids[int(head[1:].split()[0])] = [int(x) for x in rest.split()]
    return ids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="2,000 fuzz records and 3 prompts")
    ap.add_argument("--no-llama", action="store_true")
    ap.add_argument("--gen", type=int, default=12)
    a = ap.parse_args()
    out = os.path.join(ROOT, "bench-archive", time.strftime("%Y%m%d-%H%M%S") + "-psram_llm-pcverify")
    os.makedirs(out, exist_ok=True)
    rep = ["# PC verification of the Teensy core -- %s" % os.path.basename(out), ""]
    fails = 0

    def note(s):
        print(s, flush=True)
        rep.append(s)

    # ---- 1 tokenizer ------------------------------------------------------------------------------------------
    corpus = os.path.join(out, "corpus.txt")
    files = sorted(glob.glob(os.path.join(ROOT, "docs", "*.md")) + glob.glob(os.path.join(ROOT, "firmware", "bench-one", "shared", "*.[ch]")))
    with open(corpus, "wb") as f:
        for p in files:
            f.write(open(p, "rb").read())
    r = random.Random(20260927)
    kinds = list(PALETTE)
    w = [PALETTE[k][0] for k in kinds]
    nrec = 2000 if a.quick else 20000
    recs = ["".join(PALETTE[k][1](r) for k in r.choices(kinds, w, k=r.randint(1, 40))) for _ in range(nrec)]
    recpath = os.path.join(out, "fuzz.bin")
    with open(recpath, "wb") as f:
        f.write(b"\x00".join(x.encode("utf-8") for x in recs))
    note("## 1. Tokenizer")
    note("")
    for label, env, src in (("corpus lines", "TL_TOKFILE", corpus), ("fuzz records", "TL_TOKREC", recpath)):
        rc1, e1 = run("tl_ref.exe", ["x"], {env: src}, os.path.join(out, "tok_ref_%s.txt" % env))
        rc2, e2 = run("tl_host.exe", ["x"], {env: src}, os.path.join(out, "tok_host_%s.txt" % env))
        ok = rc1 == 0 and rc2 == 0 and same(os.path.join(out, "tok_ref_%s.txt" % env), os.path.join(out, "tok_host_%s.txt" % env))
        rt = [s for s in e2.splitlines() if "round-trip" in s]
        R = dump_ids(os.path.join(out, "tok_ref_%s.txt" % env))
        ntok = sum(len(v) for v in R.values())
        note("- %s: %d, %d tokens -- tl_core == tokenizer.c: **%s**; %s" % (label, len(R), ntok, "IDENTICAL" if ok else "DIFFERENT", rt[-1] if rt else "no round-trip line"))
        fails += 0 if ok and rt and " 0 round-trip" in rt[-1] else 1
        if not a.no_llama and llama_up():
            if env == "TL_TOKFILE":
                raw = open(src, "rb").read().split(b"\n")
                texts = {i: (b + b"\n" if i < len(raw) else b).decode("utf-8", "replace") for i, b in enumerate(raw, 1)}
            else:
                texts = {i: x for i, x in enumerate(recs, 1)}
            bad = [i for i in R if texts.get(i) and llama_tok(texts[i]) != R[i]]
            note("  - against llama.cpp on the same text: %d of %d differ%s" % (len(bad), len(R), (" (first %s)" % bad[:10]) if bad else ""))
            fails += 1 if bad else 0
    note("")

    # ---- 2 forward, 3 together --------------------------------------------------------------------------------
    prompts = PROMPTS[:3] if a.quick else PROMPTS
    note("## 2. Forward pass, %d prompts, %d generated tokens each" % (len(prompts), a.gen))
    note("")
    for k, p in enumerate(prompts):
        ref = os.path.join(out, "ref_%d.txt" % k)
        run("tl_ref.exe", [p, str(a.gen)], {}, ref)
        for mode, env in (("per token", {}), ("prompt batched", {"TL_PREFILL": "1"})):
            got = os.path.join(out, "host_%d_%s.txt" % (k, "b" if env else "t"))
            run("tl_host.exe", [p, str(a.gen)], env, got)
            ok = same(ref, got)
            fails += 0 if ok else 1
            note("- prompt %d, %s: %s (%d lines) -- %r" % (k, mode, "IDENTICAL" if ok else "DIFFERENT",
                                                         sum(1 for _ in open(ref, encoding="utf-8")), p[:60]))
    note("")
    note("## 3. Answered together (TL_MULTI)")
    note("")
    mfile = os.path.join(out, "multi_prompts.txt")
    with open(mfile, "w", encoding="utf-8", newline="\n") as f:
        for p in prompts:
            f.write(chat_template.escape(p) + "\n")
    for share in ("0", "1"):
        pre = os.path.join(out, "multi_s%s_" % share)
        rc, err = run("tl_host.exe", ["-", str(a.gen)], {"TL_MULTI": mfile, "TL_MULTI_OUT": pre, "TL_MULTI_SHARE": share},
                      os.path.join(out, "multi_s%s.out" % share))
        note("- shared openings %s: %s" % ("on" if share == "1" else "off",
                                           next((s for s in err.splitlines() if s.startswith("multi:")), "no summary (rc %d)" % rc)))
        for s in err.splitlines():
            if "copies its first" in s:
                note("  - " + s.strip())
        for k in range(len(prompts)):
            ok = os.path.exists(pre + "%d.txt" % k) and same(os.path.join(out, "ref_%d.txt" % k), pre + "%d.txt" % k)
            fails += 0 if ok else 1
            note("  - prompt %d together: %s" % (k, "IDENTICAL" if ok else "DIFFERENT"))
    note("")

    # ---- 3b a PSRAM bank fails mid-run --------------------------------------------------------------------------
    # tl_host's stand-in kills one bank on its n-th write (TL_PS_FAULT); the core retires it, moves its layers to
    # spare slots and the host re-runs the prompt. The lines must still equal the reference, and the host's own
    # comparison of the re-run against what it printed before the fault must say identical (exit 2 otherwise).
    note("## 3b. A PSRAM bank fails mid-run (TL_PS_FAULT: bank 3 dies on its 200th write)")
    note("")
    for mode, env in (("per token", {}), ("prompt batched", {"TL_PREFILL": "1"})):
        got = os.path.join(out, "fault_0_%s.txt" % ("b" if env else "t"))
        e = dict(env, TL_PS_FAULT="3:200")
        rc, err = run("tl_host.exe", [prompts[0], str(a.gen)], e, got)
        ok = rc == 0 and same(os.path.join(out, "ref_0.txt"), got)
        fails += 0 if ok else 1
        rec = next((s.strip() for s in err.splitlines() if s.startswith("recovery:")), "no recovery line (rc %d)" % rc)
        note("- prompt 0, %s: %s; %s" % (mode, "IDENTICAL" if ok else "DIFFERENT", rec))
    pre = os.path.join(out, "fault_multi_")
    rc, err = run("tl_host.exe", ["-", str(a.gen)], {"TL_MULTI": mfile, "TL_MULTI_OUT": pre, "TL_PS_FAULT": "3:200"},
                  os.path.join(out, "fault_multi.out"))
    n_ok = sum(1 for k in range(len(prompts))
               if os.path.exists(pre + "%d.txt" % k) and same(os.path.join(out, "ref_%d.txt" % k), pre + "%d.txt" % k))
    fails += 0 if (rc == 0 and n_ok == len(prompts)) else 1
    note("- all %d prompts together: %d identical after the fault (%s)" % (len(prompts), n_ok,
         next((s.strip() for s in err.splitlines() if "retired" in s), "no retirement line (rc %d)" % rc)))
    note("")

    # ---- 4 the Teensy's own kernels, emulated (README: dot_verify_m7.exe, tl_host_m7.exe) ---------------------
    dv, hm = os.path.join(TESTS, "dot_verify_m7.exe"), os.path.join(TESTS, "tl_host_m7.exe")
    if os.path.exists(dv) or os.path.exists(hm):
        note("## 4. The M7 kernels, emulated on this PC (GD_EMULATE_M7)")
        note("")
    if os.path.exists(dv):
        r = subprocess.run([dv, "512", "11008"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=TESTS)
        txt = r.stdout.decode("utf-8", "replace")
        bad = sum(1 for s in txt.splitlines() if "DIFFER" in s)
        fails += 1 if (bad or r.returncode) else 0
        note("- dot_verify_m7: single, presum and batched kernels against the scalar reference: %s" %
             ("IDENTICAL" if not bad and not r.returncode else "%d DIFFER lines" % bad))
    if os.path.exists(hm):
        k = 0                                    # France: per token and batched through the emulated kernels
        for mode, env in (("per token", {}), ("prompt batched", {"TL_PREFILL": "1"})):
            got = os.path.join(out, "m7_%s.txt" % ("b" if env else "t"))
            e = dict(os.environ)
            e.update(env)
            with open(got, "wb") as f:
                subprocess.run([hm, MODEL, prompts[k], str(a.gen)], stdout=f, stderr=subprocess.DEVNULL, cwd=TESTS, env=e)
            ok = same(os.path.join(out, "ref_%d.txt" % k), got)
            fails += 0 if ok else 1
            note("- tl_host_m7, prompt %d, %s: %s" % (k, mode, "IDENTICAL" if ok else "DIFFERENT"))
        if not a.quick:
            pre = os.path.join(out, "m7_multi_")
            rc, err = run("tl_host_m7.exe", ["-", str(a.gen)], {"TL_MULTI": mfile, "TL_MULTI_OUT": pre}, os.path.join(out, "m7_multi.out"))
            n_ok = sum(1 for k in range(len(prompts))
                       if os.path.exists(pre + "%d.txt" % k) and same(os.path.join(out, "ref_%d.txt" % k), pre + "%d.txt" % k))
            fails += 0 if n_ok == len(prompts) else 1
            note("- tl_host_m7, all %d prompts together (batched kernels, shared openings): %d identical" % (len(prompts), n_ok))
        note("")
    note("**%s**" % ("ALL IDENTICAL" if not fails else "%d DIFFERENCES" % fails))
    open(os.path.join(out, "report.md"), "w", encoding="utf-8").write("\n".join(rep) + "\n")
    print("report:", os.path.join(out, "report.md"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
