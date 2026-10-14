#!/bin/sh
# Measurement only (research/optimize/copyonce.md): the once-per-frame copy against today's.
# Usage: copyonce.sh GAME-ROM SET.json FRAMES [SCRIPT]   (run it inside emu-lock)
# Needs the runner built with -DDSC_COPYONCE=ON (RUNNER_CO), plus what ab.sh needs.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
rom=$1 set=$2 frames=$3 script=${4:-}
: "${RUNNER_CO:?}" "${DSC_PC:?address of dsc_capture from the module map}"
out=${OUT:?directory for the log}
mkdir -p "$out"
RUNNER=$RUNNER_CO PORT=${PORT:-$((8500 + $$ % 400))} ENABLED=1 ROM=$rom WORK=$out/work SCRIPT=$script \
  RUNNER_ARGS="--profile --profile-frames $frames --script-from-game --copyonce $out/cov.bin --copyonce-pc $DSC_PC" \
  "$repo/tests/launcher/run.sh" "$set" $((frames + ${BOOT_FRAMES:-6000})) > "$out/run.out" 2>&1 || true
