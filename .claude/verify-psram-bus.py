#!/usr/bin/env python3
"""Check that every psram_* sketch drives the bus with identical code.

WHY THIS EXISTS
---------------
The PSRAM driver was qualified by soaks covering 264 MB and then produced one to three wrong bytes
per 8 MB anyway. The settings were right. The cause was that the driver's write loop was hand-unrolled
eight bytes at a time while every test that qualified it used a plain loop, and unrolling removes the
loop branch between nibbles: at the same no-op count the driver wrote 4.4% faster, out past the margin
those tests had measured. It was never running the code that had been signed off.

A no-op count is not a timing specification. It is a timing specification for one exact instruction
sequence. So two sketches that share a number must share the code that number describes, and the only
reliable way to keep that true is to check it mechanically.

WHAT IT COMPARES
----------------
The bus layer -- put_nib, get_nib, s_byte, addr_out and the two transfer loops -- across every sketch
under firmware/bench-one/tests/psram_*/. Parts are matched by NAME, never by position, so a sketch with
an extra function cannot shift the comparison and produce a nonsense diff.

The no-op counts themselves legitimately differ between sketches, because that is exactly what the
sweeps vary, so template arguments and literal spin counts are normalised away. What survives is the
instruction sequence, which is the thing that has to match.

DELIBERATE VARIANTS
-------------------
A sketch that really does need a different bus layer says so, in a comment:

    /* BUS-VARIANT: why this one differs */

It is then reported as a known variant rather than a failure, and the reason is printed. That keeps the
check honest in both directions: an intentional difference is visible, and its numbers are known not to
transfer to the driver.

Run: python .claude/verify-psram-bus.py
Exits non-zero when an undeclared difference exists.
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# .cpp too: psram_llm keeps its bus layer in a generated .cpp so the Arduino builder cannot write
# prototypes into it, and a copy the checker never reads is a copy nobody checks.
SKETCHES = sorted(glob.glob(os.path.join(
    ROOT, 'firmware', 'bench-one', 'tests', 'psram_*', '*.ino')) +
    glob.glob(os.path.join(ROOT, 'firmware', 'bench-one', 'tests', 'psram_*', '*.cpp')))

# name in source -> label in the report. The transfer loops appear under several names across
# sketches because each generation renamed them; they are the same role and must match.
PARTS = [
    (['put_nib'], 'put_nib'),
    (['get_nib'], 'get_nib'),
    (['s_byte'], 's_byte'),
    (['addr_out'], 'addr_out'),
    (['psram_write', 'wr', 'bwrite'], 'write loop'),
    (['psram_read', 'rd', 'bread'], 'read loop'),
]


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    return s


def normalise(s):
    """Remove everything that is allowed to differ between sketches."""
    s = strip_comments(s)
    # the no-op count is the variable under test: <SW>, <SR>, <S>, <6>, <10> all become <N>
    s = re.sub(r'<\s*(?:SW|SR|S|SWR|SRD|\d+)\s*>', '<N>', s)
    s = re.sub(r'spin\s*<\s*\w+\s*>\s*\(\s*\)', 'spin<N>()', s)
    s = re.sub(r'\bspin\s*\(\s*\)', 'spin<N>()', s)          # pre-template sketches
    # burst length is swept too, and BURST vs g_burst is one quantity under two names
    s = re.sub(r'\bg_burst\b|\bBURST\b', 'BURSTLEN', s)
    s = re.sub(r'\b96u?\b', 'BURSTLEN', s)
    # the base-word helpers differ in which bit they deassert, which is selection and not timing
    s = re.sub(r'\b(?:qbase|cbase)\b', 'BASE', s)
    # the transfer entry points carry different names for the same body
    s = re.sub(r'\b(?:psram_write|wr|bwrite)\b', 'XFER_W', s)
    s = re.sub(r'\b(?:psram_read|rd|bread)\b', 'XFER_R', s)
    s = re.sub(r'\s+', ' ', s)
    return s.strip()


def extract(src, name):
    """Pull a function or template body out by brace matching, not by regex alone."""
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', src):
        open_brace = src.find('{', m.end())
        if open_brace < 0:
            continue
        semi = src.find(';', m.end())
        if 0 <= semi < open_brace:
            continue                        # a declaration or a call, not a definition
        depth, j = 0, open_brace
        while j < len(src):
            if src[j] == '{':
                depth += 1
            elif src[j] == '}':
                depth -= 1
                if depth == 0:
                    return src[m.start():j + 1]
            j += 1
    return None


def main():
    if not SKETCHES:
        print('  no psram sketches found -- is the tree laid out as expected?')
        return 1

    # label -> { normalised body : [sketches] },  plus declared variants
    table = {label: {} for _, label in PARTS}
    variants = {}
    seen = 0

    for path in SKETCHES:
        raw = open(path, encoding='utf-8', errors='replace').read()
        rel = os.path.relpath(path, ROOT).replace('\\', '/')
        v = re.search(r'BUS-VARIANT:\s*([^\n*]+)', raw)
        if v:
            variants[rel] = v.group(1).strip()
        src = strip_comments(raw)
        found = False
        for names, label in PARTS:
            for n in names:
                body = extract(src, n)
                if body:
                    table[label].setdefault(normalise(body), []).append(rel)
                    found = True
                    break
        if found:
            seen += 1

    print('  comparing the bus layer across %d sketches' % seen)
    undeclared = 0
    for _, label in PARTS:
        groups = table[label]
        if not groups:
            continue
        if len(groups) == 1:
            print('  ok      %-11s %d sketches agree' % (label, len(next(iter(groups.values())))))
            continue
        ordered = sorted(groups.items(), key=lambda kv: -len(kv[1]))
        canon_files = ordered[0][1]
        odd = [f for body, files in ordered[1:] for f in files]
        bad = [f for f in odd if f not in variants]
        print('  %s %-11s %d agree, differs in: %s' % (
            'VARIANT' if not bad else 'DIFFER ', label, len(canon_files), ', '.join(odd)))
        for f in odd:
            if f in variants:
                print('            declared variant: %s -- %s' % (f, variants[f]))
        if bad:
            undeclared += 1
            for body, files in ordered[1:]:
                for f in files:
                    if f in bad:
                        print('            UNDECLARED: %s' % f)
                        print('              %s' % body[:200])

    if undeclared:
        print('\n  %d part(s) differ without a declared reason. Either the sketch is a deliberate'
              % undeclared)
        print('  experiment -- say so with a /* BUS-VARIANT: ... */ comment, and know that its')
        print('  numbers do not transfer to the driver -- or this is the bug from docs/41 happening')
        print('  again. The 4.4% that unrolling bought was invisible in every sweep for an afternoon.')
        return 1

    print('\n  no undeclared differences: every sketch that shares a no-op count shares the')
    print('  instruction sequence that number describes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
