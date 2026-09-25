#!/bin/bash
# build_and_run.sh -- compile each firmware's real .ino against the host shims and run its test.
# Run inside WSL (g++). Outputs land in firmware/tests/host/out/.
set -u
H=$(cd "$(dirname "$0")" && pwd); O=$H/out; mkdir -p "$O"; cd "$O"
rc=0
for t in vent_display climate_node can_logger; do
  if g++ -std=c++17 -O1 -w -I"$H/shim" -I"$H" -x c++ "$H/test_$t.cpp" -o "$O/test_$t" 2>"$O/build_$t.log"; then
    "$O/test_$t" || rc=1
  else
    echo "BUILD FAILED: $t"; head -25 "$O/build_$t.log"; rc=1
  fi
done
exit $rc
