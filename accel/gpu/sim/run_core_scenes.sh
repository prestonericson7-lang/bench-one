#!/bin/bash
# run_core_scenes.sh -- generate every golden-model scene (sim/golden.py) into a scratch directory and
# run the render core (tb_core, Icarus) on all of them, bit-exact against the model. WSL, any user.
#   bash sim/run_core_scenes.sh [scene ...]      (default: all scenes)
set -u
G=$(cd "$(dirname "$0")/.." && pwd); cd "$G"
S=${SCENES_DIR:-/tmp/gpu_scenes}
rm -rf "$S"; mkdir -p "$S"
python3 sim/golden.py gen "$S" "$@" >/dev/null || { echo "scene generation failed"; exit 1; }
dirs=$(ls -d "$S"/*/)
echo "scenes: $(echo "$dirs" | wc -l) in $S"
bash sim/run_core.sh $dirs
rc=$?
rm -rf "$S"
exit $rc
