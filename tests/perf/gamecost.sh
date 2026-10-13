#!/bin/sh
# What DScheevos costs the GAME: the game's own ARM9 cycles per VBlank interval with
# achievements off vs on (same input), with the cache model and --thumb-cycle. Runs
# ab.sh (two emulator runs) and prints gamecost.py's table: off, on, the delta
# (avg/median/p90/max), game CPI, the game's cache misses, and the shadow-cache
# estimate of what our cache use costs it (I/D split). See tests/perf/AB.md.
# Usage: gamecost.sh GAME [FRAMES] [SCRIPT]
#   GAME    kirby, pokemon, nsmb or a ROM file (as ab.sh); FRAMES default 1500
# Env: everything ab.sh takes (SET, MODULE, BOOT_FRAMES, ...), plus SKIP=N (leave out
#   the first N intervals, e.g. menus), OUT=dir (keep both profile logs there),
#   JSON=1 (gamecost.py --json), PORT (mock server; default from the PID). Wrap it in emu-lock like any emulator run.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 1 ] || { sed -n '2,13p' "$0"; exit 2; }
game=$1 frames=${2:-1500} script=${3:-}
dir=${OUT:-$(mktemp -d)}
[ -n "${OUT:-}" ] || trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir"
# a port of its own: two lanes' mock servers on ab.sh's default port make the launcher fail
# ("nds-bootstrap didn't use the session")
PORT=${PORT:-$((8500 + $$ % 400))} PROFDIR=$dir EXTRA_ARGS="--thumb-cycle ${EXTRA_ARGS:-}" CACHE_MODEL=1 MODES="off on" \
  "$here/ab.sh" "$game" "$frames" $script > "$dir/ab.txt" 2>&1 || { tail -20 "$dir/ab.txt"; exit 1; }
grep "^FAIL\|^server saw" "$dir/ab.txt" || true
python3 "$here/gamecost.py" ${SKIP:+--skip $SKIP} ${JSON:+--json} "$dir/prof.off.csv" "$dir/prof.on.csv"
