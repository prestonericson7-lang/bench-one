#!/usr/bin/env python3
"""compare_run.py -- check a psram_llm serial log against the PC reference, step by step.

    python compare_run.py <serial.log> [expected_france.txt]

The PC reference (tests/tl_ref.c, shared/model_q.c's fused path) and the Teensy print the same line per
position. Token ids are compared exactly. Logits are compared numerically: the Teensy's libm and the PC's
differ in expf/powf/sinf/cosf, so the last bits can move and a near-tie can flip. When the ids diverge,
this reports the step and the reference's own margin between its top two there, which says whether it was
a near-tie. After a divergence the two runs are feeding different tokens and are no longer comparable.

Exit 0: every compared step has the same top1 id. Exit 1: ids diverge. Exit 2: nothing to compare."""
import re
import sys

LINE = re.compile(r'step (\d+) pos (\d+) fed (-?\d+) -> top1 (-?\d+) (\S+) top2 (-?\d+) (\S+) text "(.*)"')


def load(path):
    steps, prompt = {}, None
    with open(path, 'rb') as f:
        for raw in f:
            s = raw.decode('utf-8', 'replace').strip()
            if s.startswith('prompt ids:'):
                prompt = s.split(':', 1)[1].split()
            m = LINE.search(s)
            if m:
                k = int(m.group(1))
                steps[k] = dict(fed=int(m.group(3)), t1=int(m.group(4)), l1=float(m.group(5)),
                                t2=int(m.group(6)), l2=float(m.group(7)), text=m.group(8))
    return prompt, steps


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    exp_path = sys.argv[2] if len(sys.argv) > 2 else __file__.replace('compare_run.py', 'expected_france.txt')
    pe, E = load(exp_path)
    pt, T = load(sys.argv[1])
    if not T:
        print('no step lines in %s' % sys.argv[1])
        return 2
    if pe != pt:
        print('PROMPT IDS DIFFER\n  reference %s\n  teensy    %s' % (pe, pt))
        return 1
    print('prompt ids identical: %s' % ' '.join(pt))
    print('%5s  %-22s %-22s %12s %12s' % ('step', 'reference top1', 'teensy top1', 'logit delta', 'ref margin'))
    worst = 0.0
    n = 0
    for k in sorted(T):
        if k not in E:
            break
        e, t = E[k], T[k]
        if e['fed'] != t['fed']:
            print('  step %d fed different tokens (%d vs %d): the runs had already diverged' % (k, e['fed'], t['fed']))
            break
        d = t['l1'] - e['l1']
        margin = e['l1'] - e['l2']
        same = e['t1'] == t['t1']
        print('%5d  %-22s %-22s %+12.3e %12.4f%s' % (
            k, '%d %r' % (e['t1'], e['text']), '%d %r' % (t['t1'], t['text']), d, margin,
            '' if same else '   <-- DIVERGED'))
        n += 1
        if not same:
            print('\nDIVERGED at step %d. Reference margin between its top two there: %.4f.' % (k, margin))
            print('Largest top1 logit difference before it: %.3e.' % worst)
            return 1
        worst = max(worst, abs(d))
    print('\nALL %d compared steps choose the same token. Largest top1 logit difference: %.3e.' % (n, worst))
    return 0


if __name__ == '__main__':
    sys.exit(main())
