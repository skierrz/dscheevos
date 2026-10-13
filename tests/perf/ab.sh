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
#   PROFDIR=dir (keep each run's profile log as dir/prof.off.csv and prof.on.csv).
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

# What the GAME costs (PC outside the module; runner columns game*, see --profile): its own
# ARM9 cycles per interval with achievements off and on, and the shadow-cache estimate of
# what sharing the caches with the module costs it (on; 0 off by construction).
rows = {}
for mode in ("off", "on"):
    f = "%s/%s/prof.csv" % (top, mode)
    rows[mode] = list(csv.DictReader(open(f))) if os.path.exists(f) else []
if all(rows[m] and "game" in rows[m][0] for m in ("off", "on")):
    def pctl(v, q):
        v = sorted(v)
        return v[min(len(v) - 1, int(q * len(v)))] if v else 0
    def dist(v):
        return "%8.0f %8.0f %8.0f %8.0f" % (sum(v) / max(len(v), 1), pctl(v, 0.5), pctl(v, 0.99), max(v or [0]))
    g = {m: [(int(r["game"]) + int(r["game_irq"])) / 2 for r in rows[m]] for m in rows}
    gi = {m: [int(r["game_irq"]) / 2 for r in rows[m]] for m in rows}
    ins = {m: [int(r["game_instr"]) for r in rows[m]] for m in rows}
    print("\nthe game's own ARM9 cycles per interval (67 MHz)     avg   median      p99      max")
    for m in ("off", "on"):
        print("  game, achievements %-3s                    %s" % (m, dist(g[m])))
        print("    of it in IRQ mode                        %s" % dist(gi[m]))
        print("    instructions                             %s" % dist(ins[m]))
    n = min(len(g["off"]), len(g["on"]))
    d = [g["on"][i] - g["off"][i] for i in range(n)]
    dn = [ins["on"][i] - ins["off"][i] for i in range(n)]
    same = sum(1 for x in dn if x == 0)
    print("  on - off, same interval                    %s" % dist(d))
    print("    (min %.0f; intervals with the same game instruction count: %d of %d)" % (min(d or [0]), same, n))
    cpi = {m: sum(g[m][:n]) / max(sum(ins[m][:n]), 1) for m in rows}
    print("  game CPI off %.4f on %.4f (%+.2f%%)" % (cpi["off"], cpi["on"], 100 * (cpi["on"] / cpi["off"] - 1) if cpi["off"] else 0))
    def col(m, k): return [int(r[k]) for r in rows[m]]
    for k, name in (("game_imiss", "game I-cache misses"), ("game_dreadmiss", "game D-cache read misses"),
                    ("game_writeback", "game dirty write-backs")):
        print("  %-27s off %8.1f on %8.1f per interval" % (name, sum(col("off", k)) / max(len(rows["off"]), 1),
                                                      sum(col("on", k)) / max(len(rows["on"]), 1)))
    ex = [(int(r["game_iextra"]) + int(r["game_dextra"])) / 2 for r in rows["on"]]
    exi = [int(r["game_iextra"]) / 2 for r in rows["on"]]
    exd = [int(r["game_dextra"]) / 2 for r in rows["on"]]
    print("  shadow-cache cost (on): game cycles lost to the module's cache use      avg   median      p99      max")
    print("    I + D                                    %s" % dist(ex))
    print("    I-cache                                  %s" % dist(exi))
    print("    D-cache                                  %s" % dist(exd))
    print("    misses caused per interval: I %.1f, D %.1f, write-backs %.1f" % tuple(
        sum(col("on", k)) / max(len(rows["on"]), 1) for k in ("poll_i", "poll_d", "poll_wb")))
    a7 = {m: col(m, "arm7_busy") for m in rows}
    if any(a7["on"]):
        print("  ARM7 busy cycles per interval (33 MHz)     off %s" % dist(a7["off"]))
        print("                                             on  %s" % dist(a7["on"]))
EOF
