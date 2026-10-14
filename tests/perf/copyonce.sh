#!/bin/sh
# Measurement only (research/optimize/copyonce.md, copyonce2.md): logs the game's RAM at the module's
# snapshot copy and at the other copy points, for the replay (copyonce.c).
# Usage: copyonce.sh GAME-ROM SET.json FRAMES [SCRIPT]   (run it inside emu-lock, with tests/perf/hardtimeout)
# Needs the runner built with -DDSC_COPYONCE=ON (RUNNER_CO), plus what tests/launcher/run.sh needs
# (DSI_FILES, BOOTSTRAP, built module/launcher). The address of dsc_capture is read from the module map
# (DSC_PC overrides). SAVE=file starts the game from that save (with SD_SETUP=tests/perf/copyonce-sav.sh).
# OUT=directory for cov.bin; the private work directory (token, SD image) is deleted on exit.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
rom=$1 set=$2 frames=$3 script=${4:-}
: "${RUNNER_CO:?}"
out=${OUT:?directory for the log}
mod=${MODULE:-$repo/dscheevos/module/build}
pc=${DSC_PC:-0x$(awk '$2=="dsc_capture"{print substr($1,3)}' "$mod/dscheevos9.map" | head -1)}
. "$here/../lib/workdir.sh"
mkdir -p "$out"
work=$(dsc_mkwork "co2-p${PORT:-0}")
trap 'exit 130' INT TERM HUP
trap 'dsc_rmwork "$work"' EXIT
RUNNER=$RUNNER_CO PORT=${PORT:-$((8500 + $$ % 400))} ENABLED=1 ROM=$rom WORK=$work SCRIPT=$script \
  RUNNER_ARGS="--profile --profile-frames $frames --script-from-game --copyonce $out/cov.bin --copyonce-pc $pc ${EXTRA_ARGS:-}" \
  "$repo/tests/launcher/run.sh" "$set" $((frames + ${BOOT_FRAMES:-6000})) > "$out/run.out" 2>&1 || true
# the work dir holds the SD image and session files: only a redacted tail of the launcher output is kept
sed -E 's/[A-Za-z0-9]{24,}/<redacted>/g' "$out/run.out" > "$out/run.out.red"; mv "$out/run.out.red" "$out/run.out"
[ -f "$work/screen.ppm" ] && cp "$work/screen.ppm" "$out/screen.ppm" || true
echo "pc $pc"; ls -l "$out/cov.bin" | awk '{print $5, "bytes"}'
