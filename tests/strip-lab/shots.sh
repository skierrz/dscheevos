#!/bin/bash
# LAB ONLY: run.sh with a screenshot every STEP frames from FROM to TO, plus --profile; then a contact sheet.
#   tests/strip-lab/shots.sh BUILD_DIR ROM FRAMES FROM TO STEP WORK
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=$1 rom=$2 frames=$3 from=$4 to=$5 step=$6 work=$7
sh=""; for f in $(seq "$from" "$step" "$to"); do sh="$sh$f SHOT\n"; done
rm -rf "$work"
EXTRA_SCRIPT="${EXTRA_SCRIPT:-}$sh" RUNNER_ARGS="--profile ${RUNNER_ARGS:-}" "$here/run.sh" "$src" "$rom" "$frames" "" "$work" ${EXPECT:---expect 9901} 2>&1 \
  | grep -E "RESULT|strip:|\[prof\] lag|cycles/interval outside" || true
python3 "$here/sheet.py" "$work/main" "$work/sheet.png" && echo "sheet: $work/sheet.png"
