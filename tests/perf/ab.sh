#!/bin/sh
# Stock vs DScheevos A/B in melonDS (see tests/perf/AB.md): the same game and
# input through tests/launcher/run.sh twice, achievements off (ENABLED = 0 in
# dscheevos.ini, so nds-bootstrap starts the game without the module) and on.
# Prints lag intervals, intervals and module cycles for each.
# Usage: ab.sh GAME FRAMES [SCRIPT]
#   GAME    kirby, pokemon, nsmb (ROM_KIRBY, ROM_POKEMON, ROM_NSMB) or a ROM file
#   FRAMES  VBlank intervals to measure, from the game's first OS_Halt call
#   SCRIPT  runner input script; its frame numbers count from that call too
# Optional: SET=file.json (default: the synthetic typical set, gen_set.py 60 8 1 3),
#   CACHE_MODEL=0 (melonDS's flat cache timing), MODULE=dir (another module build),
#   KEEP=1 (keep the work folders, 450-650 MB each), BOOT_FRAMES (frame cap before the game, 6000),
#   PORT (mock server, default 8453), PCS=file (the VBlank call by PC and its cache misses,
#   achievements on, see the runner's --profile-pcs).
#   MODES="on" (skip the stock run; the comparison table needs both),
#   EXTRA_ARGS (more runner arguments, both runs), GAMEPCS=prefix (the game's extra cycles from
#   sharing the caches, by PC: prefix.on.csv/.off.csv), ARM7=prefix (ARM7 cycles by PC block),
#   SKIP=N (game-cost table: leave out the first N intervals), PROFDIR=dir (keep each run's profile log as dir/prof.off.csv and prof.on.csv).
# Needs what tests/launcher/run.sh needs (DSI_FILES, BOOTSTRAP, RUNNER). Runs the
# emulator twice, one after the other; don't run other emulator tests meanwhile.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
[ $# -ge 2 ] || { sed -n '2,16p' "$0"; exit 2; }
game=$1 frames=$2 script=${3:-}
case $game in
  kirby) rom=${ROM_KIRBY:?} ;;
  pokemon) rom=${ROM_POKEMON:?} ;;
  nsmb) rom=${ROM_NSMB:?} ;;
  *) rom=$game ;;
esac
[ -f "$rom" ] || { echo "no ROM: $rom"; exit 2; }
[ -z "$script" ] || script=$(cd "$(dirname "$script")" && pwd)/$(basename "$script")

top=$(mktemp -d)
[ "${KEEP:-0}" = 1 ] || trap 'rm -rf "$top"' EXIT
set=${SET:-}
if [ -z "$set" ]; then
  set=$top/typical.json
  python3 "$repo/tools/rcheevos-footprint/harness/gen_set.py" 60 8 1 3 | sed -n 2p |
    python3 -c 'import json,sys; s=sys.stdin.read(); print(json.loads(s[s.index("=")+1:].strip().rstrip(";")))' > "$set"
fi
args="--profile --profile-frames $frames --script-from-game"
[ "${CACHE_MODEL:-1}" = 0 ] || args="$args --cache-model-from-game"

for mode in ${MODES:-off on}; do
  work=$top/$mode
  mkdir -p "$work"
  enabled=0; [ $mode = on ] && enabled=1
  extra=${EXTRA_ARGS:-}
  [ $mode = on ] && [ -n "${PCS:-}" ] && extra="$extra --profile-pcs $PCS"
  [ -n "${GAMEPCS:-}" ] && extra="$extra --profile-game-pcs $GAMEPCS.$mode.csv"
  [ -n "${ARM7:-}" ] && extra="$extra --profile-arm7 $ARM7.$mode.csv"
  echo "== $(basename "$rom"): achievements $mode"
  PORT=${PORT:-8453} ENABLED=$enabled ROM=$rom WORK=$work SCRIPT=$script RUNNER_ARGS="$args --profile-log $work/prof.csv $extra" \
    "$repo/tests/launcher/run.sh" "$set" $((frames + ${BOOT_FRAMES:-6000})) > "$work/run.out" 2>&1 || true
  grep "^\[prof\]\|^\[runner\]\|^\[game\]\|^\[arm7\]\|^\[cache\]" "$work/runner.log" || { echo "no profile; see $work/run.out"; exit 1; }
  [ -z "${PROFDIR:-}" ] || cp "$work/prof.csv" "$PROFDIR/prof.$mode.csv"
  if [ $mode = on ]; then
    [ -f "$work/sd/_nds/dscheevos/session.used" ] || { echo "FAIL: nds-bootstrap didn't use the session"; exit 1; }
    grep -q "OS_Halt ->" "$work/runner.log" || { echo "FAIL: no module in RAM"; exit 1; }
    echo "server saw $(grep -c '"unlock"' "$work/events.jsonl" || true) unlocks"
    # the module's own counters (frames snapshotted, dropped, evaluated; self-timing)
    grep "^{'\|^progress\|^self-timing\|^  IRQ \|^  idle " "$work/run.out" || true
  fi
done

python3 - "$top" "$frames" <<'EOF'
import csv, os, sys
top, want = sys.argv[1], int(sys.argv[2])
cols = {}
for mode in ("off", "on"):
    if not os.path.exists("%s/%s/prof.csv" % (top, mode)):
        cols[mode] = ["-"] * 7
        continue
    rows = list(csv.DictReader(open("%s/%s/prof.csv" % (top, mode))))
    n = len(rows)
    lag = sum(int(r["lag"]) for r in rows)
    run = best = 0
    for r in rows:
        run = run + 1 if r["lag"] == "1" else 0
        best = max(best, run)
    frame = 560190 * 4   # profile counts in 133 MHz units; a frame is 560190 cycles at 33.5 MHz
    def stat(k):
        v = [int(r[k]) for r in rows] or [0]
        return "%.0f / %.0f (%.2f%% / %.2f%%)" % (sum(v) / len(v) / 2, max(v) / 2, 100 * sum(v) / len(v) / frame, 100 * max(v) / frame)
    cols[mode] = [str(n) + ("" if n == want else " (asked %d)" % want),
                  "%d (%.2f%%)" % (lag, 100 * lag / max(n, 1)), str(best),
                  stat("module_vblank"), stat("module_idle"), stat("module_irq_mode"),
                  "%.1f%%" % (100 * sum(int(r["busy"]) for r in rows) / max(n, 1) / frame)]
names = ["intervals", "lag intervals", "longest lag run", "module VBlank call avg / max",
         "module idle work avg / max", "module in IRQ mode avg / max", "ARM9 busy"]
print("\n%-31s %-34s %s" % ("", "achievements off", "achievements on"))
for i, name in enumerate(names):
    print("%-31s %-34s %s" % (name, cols["off"][i], cols["on"][i]))
print("(cycles at 67 MHz per VBlank interval; percentages of a frame)")

EOF
if [ -f "$top/off/prof.csv" ] && [ -f "$top/on/prof.csv" ] && head -1 "$top/on/prof.csv" | grep -q ",game,"; then
  echo
  python3 "$here/gamecost.py" ${SKIP:+--skip $SKIP} "$top/off/prof.csv" "$top/on/prof.csv"
fi
