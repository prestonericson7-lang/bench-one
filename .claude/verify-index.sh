#!/bin/sh
# Check INDEX.md against the actual tree: counts, and every relative link.
#
# An index that drifts from the repository is worse than no index, because it gets believed. This
# caught two counts that were already stale minutes after INDEX.md was written, from moves made in
# the same session. Run it after moving or adding files.
#
# Comparison is by FIXED STRING, not regex. An earlier version parsed the numbers back out of the
# prose with regexes and silently extracted nothing for half the checks, reporting "index says <blank>"
# and passing things it had not actually looked at.
set -e
cd "$(dirname "$0")/.."
fail=0

want() {   # want <label> <exact line that must appear in INDEX.md>
  if grep -qF "$2" INDEX.md; then printf '  ok    %s\n' "$1"
  else printf '  STALE %s\n        expected to find: %s\n' "$1" "$2"; fail=1; fi
}

n_tracked=$(git ls-files | wc -l | tr -d ' ')
n_docs=$(ls docs/[0-9]*.md | wc -l | tr -d ' ')
n_res=$(git ls-files docs/research | wc -l | tr -d ' ')
n_fw=$(git ls-files firmware | wc -l | tr -d ' ')
n_sh=$(git ls-files firmware/bench-one/shared | wc -l | tr -d ' ')
n_ts=$(git ls-files firmware/bench-one/tests | wc -l | tr -d ' ')
n_fp=$(git ls-files firmware/bench-one/fpga | wc -l | tr -d ' ')
n_run=$(git ls-files run | wc -l | tr -d ' ')
n_hw=$(git ls-files hardware | wc -l | tr -d ' ')
n_td=$(git ls-files firmware/bench-one/tests | grep -o '^firmware/bench-one/tests/[^/]*/' | sort -u | grep -vc results)
n_tc=$(git ls-files firmware/bench-one/tests | grep -c '\.c$')
n_tl=$(git ls-files firmware/bench-one/tests/results | grep -c '\.log$')

echo "  --- counts ---"
want "tracked files"     "**$n_tracked tracked files.**"
want "docs + research"   "docs/         $n_docs numbered documents + $n_res research files"
want "firmware"          "firmware/     $n_fw files"
want "hardware"          "hardware/     $n_hw files"
want "run"               "run/          $n_run files"
want "shared"            "### \`bench-one/shared/\` — $n_sh files"
want "tests"             "### \`bench-one/tests/\` — $n_ts files"
want "fpga"              "### \`bench-one/fpga/\` — $n_fp files"
want "tests breakdown"   "**$n_td experiment directories, $n_tc C programs, $n_tl result logs.**"
want "numbered docs"     "**$n_docs numbered documents**"
want "result logs"       "**$n_tl logs.**"

echo "  --- relative links ---"
for f in INDEX.md README.md docs/README.md HARDWARE-SAFETY.md \
         firmware/bench-one/fpga/ddr3_ice40/README.md docs/38-ddr3-on-a-microcontroller.md; do
  d=$(dirname "$f")
  for L in $(grep -o '](\([^)h][^)]*\))' "$f" 2>/dev/null | sed 's/](//;s/)$//'); do
    t="${L%%#*}"
    [ -z "$t" ] && continue
    if [ ! -e "$d/$t" ]; then printf '  BROKEN %s -> %s\n' "$f" "$t"; fail=1; fi
  done
done
[ "$fail" = 0 ] && printf '  %s\n' "all links resolve"

echo
if [ "$fail" = 0 ]; then echo "  INDEX.md matches the tree"; else echo "  INDEX.md IS STALE"; exit 1; fi
