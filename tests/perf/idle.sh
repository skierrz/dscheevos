#!/bin/sh
# Idle budget: how much of each VBlank interval a game leaves the ARM9 idle,
# stock (achievements off), in melonDS with the cache model on. Runs the game
# through tests/launcher/run.sh with ENABLED = 0, profiles FRAMES intervals from
# its first OS_Halt call and writes the per-interval CSV (with halted cycles),
# screenshots and RAM dumps the input script asks for (SHOT, DUMP), then the
# idle report (idle_report.py).
# Usage: idle.sh ROM FRAMES OUT [SCRIPT]
#   OUT     folder for prof.csv, runner.log, report.txt and screenshots (PNG,
#           f<FRAME>.png; screen.png is the last frame)
#   SCRIPT  runner input script, frames from the game's first OS_Halt call:
#           "F KEYMASK HOLD", "F TOUCH X Y HOLD", "F SHOT", "F DUMP"
# Optional: DUMPS=dir (where RAM dumps go, default OUT; keep them out of the repo),
#   (RAM dumps are named f<FRAME>.bin, end.bin for the last frame)
#   SEGMENTS="name:from-to ..." (report parts of the run too, in intervals),
#   BOOT_FRAMES (frame cap before the game, 6000), PORT (mock server, 8453),
#   CACHE_MODEL=0, DSI_MODE (default 0), EXTRA_ARGS (more runner arguments).
# Needs the runner from tools/melonDS branch dscheevos-idle-budget or later
# (halted cycles in the CSV; SHOT, DUMP, TOUCH in scripts). Wrap it in emu-lock.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
[ $# -ge 3 ] || { sed -n '2,19p' "$0"; exit 2; }
rom=$1 frames=$2 out=$3 script=${4:-}
[ -f "$rom" ] || { echo "no ROM: $rom"; exit 2; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
dumps=${DUMPS:-$out}
mkdir -p "$dumps"
[ -z "$script" ] || script=$(cd "$(dirname "$script")" && pwd)/$(basename "$script")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
args="--profile --profile-frames $frames --script-from-game --profile-log $work/prof.csv"
[ "${CACHE_MODEL:-1}" = 0 ] || args="$args --cache-model-from-game"
PORT=${PORT:-8453} ENABLED=0 ROM=$rom WORK=$work SCRIPT=$script RUNNER_ARGS="$args ${EXTRA_ARGS:-}" \
  "$repo/tests/launcher/run.sh" "$repo/tests/bootstrap/sets/live.json" $((frames + ${BOOT_FRAMES:-6000})) > "$work/run.out" 2>&1 || true
cp "$work/run.out" "$out/run.out"
[ -f "$work/runner.log" ] || { echo "runner didn't run; see $out/run.out"; tail -5 "$out/run.out"; exit 1; }
cp "$work/runner.log" "$out/runner.log"
[ -f "$work/prof.csv" ] || { echo "no profile; see $out/runner.log"; exit 1; }
cp "$work/prof.csv" "$out/prof.csv"
for f in "$work"/screen.ppm*; do
  [ -f "$f" ] || continue
  b=$(basename "$f" .ppm | sed 's/^screen\.ppm\.//')
  python3 "$here/ppm2png.py" "$f" "$out/$b.png"
done
[ ! -f "$work/ram.bin" ] || mv "$work/ram.bin" "$dumps/end.bin"
for f in "$work"/ram.bin.f*.bin; do
  [ -f "$f" ] || continue
  mv "$f" "$dumps/$(basename "$f" | sed 's/^ram\.bin\.//')"
done
grep "^\[prof\]\|^\[runner\]" "$out/runner.log" || true
python3 "$here/idle_report.py" "$out/prof.csv" ${SEGMENTS:-} | tee "$out/report.txt"
