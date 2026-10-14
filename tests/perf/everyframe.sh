#!/bin/sh
# Measurement only (research/optimize/everyframe.md): logs, per frame, the game's RAM where a libretro
# frontend evaluates (end of line 262), at VBlank start, at the module's first capture after the VBlank,
# and what the module itself evaluates, for ef2cov + copyonce.c.
# Usage: everyframe.sh GAME-ROM SET.json FRAMES [SCRIPT]   (inside emu-lock + tests/perf/hardtimeout)
# Needs the runner built with -DDSC_COPYONCE=ON (RUNNER_CO), plus what tests/launcher/run.sh needs.
# Cache model and --thumb-cycle on (CACHE_MODEL=0 turns the model off). OUT=directory for evf.bin.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
rom=$1 set=$2 frames=$3 script=${4:-}
: "${RUNNER_CO:?}"
out=${OUT:?directory for the log}
mod=${MODULE:-$repo/dscheevos/module/build}
cpc=0x$(awk '$2=="dsc_capture"{print substr($1,3)}' "$mod/dscheevos9.map" | head -1)
qpc=0x$(awk '$2=="dsc_vblank_queue"{print substr($1,3)}' "$mod/dscheevos9.map" | head -1)
[ "$cpc" != 0x ] && [ "$qpc" != 0x ] || { echo "no dsc_capture/dsc_vblank_queue in $mod/dscheevos9.map"; exit 2; }
cm="--cache-model-from-game --thumb-cycle"; [ "${CACHE_MODEL:-1}" = 0 ] && cm=
. "$here/../lib/workdir.sh"
mkdir -p "$out"
work=$(dsc_mkwork "ef-p${PORT:-0}")
trap 'exit 130' INT TERM HUP
trap 'dsc_rmwork "$work"' EXIT
RUNNER=$RUNNER_CO PORT=${PORT:-$((8500 + $$ % 400))} ENABLED=1 ROM=$rom WORK=$work SCRIPT=$script \
  RUNNER_ARGS="--profile --profile-frames $frames --script-from-game $cm --profile-log $out/prof.csv --everyframe $out/evf.bin --ef-capture-pc $cpc --ef-queue-pc $qpc ${EXTRA_ARGS:-}" \
  "$repo/tests/launcher/run.sh" "$set" $((frames + ${BOOT_FRAMES:-6000})) > "$out/run.out" 2>&1 || true
grep "^\[everyframe\]\|^\[prof\]" "$work/runner.log" > "$out/runner.summary" 2>/dev/null || true
# the work dir holds the SD image and session files: only a redacted copy of the launcher output is kept
sed -E 's/[A-Za-z0-9+\/=_-]{20,}/<redacted>/g' "$out/run.out" > "$out/run.out.red"; mv "$out/run.out.red" "$out/run.out"
echo "capture pc $cpc queue pc $qpc"; cat "$out/runner.summary"; ls -l "$out/evf.bin" | awk '{print $5, "bytes"}'
