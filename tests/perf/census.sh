#!/bin/sh
# Wait census: where a game's ARM9 waits, stock (achievements off), in melonDS.
# Runs the game through tests/launcher/run.sh with ENABLED = 0 and the runner's
# --wait-census from the frame the game's header appears; writes, in OUT:
#   frames.csv  per VBlank interval: busy and halted cycles, halted split into the
#               SDK's OS_Halt, BIOS waits (SWI 4/5/6) and other CP15 halts
#   sites.csv   each halt site (kind, PC, caller LR, halts, cycles)
#   hot.csv     PC samples while the ARM9 runs (finds polling loops)
#   io.csv      PCs reading DISPSTAT / VCOUNT / IE / IF
#   report.txt  census_report.py's summary; f<FRAME>.png screenshots the script asks for
# Usage: census.sh ROM FRAMES OUT [SCRIPT]
#   SCRIPT  runner input script, frames counted from the game's header (idle_script.py)
# Optional: LAUNCHER_RUN (run.sh of a checkout with the launcher and module built), CACHE_MODEL=1, BOOT_FRAMES (frames before the game, 6000), PORT, DSI_MODE,
#   SEGMENTS="name:from-to ..." (passed to census_report.py), EXTRA_ARGS.
# Needs the runner from tools/melonDS branch dscheevos-wait-census. Wrap it in emu-lock.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
[ $# -ge 3 ] || { sed -n '2,16p' "$0"; exit 2; }
rom=$1 frames=$2 out=$3 script=${4:-}
[ -f "$rom" ] || { echo "no ROM: $rom"; exit 2; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
[ -z "$script" ] || script=$(cd "$(dirname "$script")" && pwd)/$(basename "$script")
code=$(dd if="$rom" bs=1 skip=12 count=4 2>/dev/null)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
args="--wait-census $work/census --census-code $code --census-frames $frames --script-from-game"
[ "${CACHE_MODEL:-0}" = 0 ] || args="$args --cache-model"
PORT=${PORT:-8453} ENABLED=0 ROM=$rom WORK=$work SCRIPT=$script RUNNER_ARGS="$args ${EXTRA_ARGS:-}" \
  "${LAUNCHER_RUN:-$repo/tests/launcher/run.sh}" "$repo/tests/bootstrap/sets/live.json" $((frames + ${BOOT_FRAMES:-6000})) > "$work/run.out" 2>&1 || true
cp "$work/run.out" "$out/run.out"
[ -f "$work/runner.log" ] || { echo "runner didn't run; see $out/run.out"; tail -5 "$out/run.out"; exit 1; }
cp "$work/runner.log" "$out/runner.log"
for k in frames sites hot io; do
  [ -f "$work/census.$k.csv" ] && cp "$work/census.$k.csv" "$out/$k.csv"
done
for f in "$work"/screen.ppm*; do
  [ -f "$f" ] || continue
  b=$(basename "$f" .ppm | sed 's/^screen\.ppm\.//')
  python3 "$here/ppm2png.py" "$f" "$out/$b.png"
done
grep "^\[census\]\|^\[runner\]" "$out/runner.log" || true
[ -f "$out/frames.csv" ] || { echo "no census; see $out/runner.log"; exit 1; }
python3 "$here/census_report.py" "$out" ${SEGMENTS:-} | tee "$out/report.txt"
