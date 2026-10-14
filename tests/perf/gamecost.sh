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
repo=$here/../..
# What this run measures with, printed first: two agents measured "the same code" 3k-11k cycles apart
# because one used a runner binary built before the runner submodule's last commits (a stale build in
# another checkout). Compare these lines before comparing numbers.
sha() { if [ -f "$1" ]; then shasum "$1" | cut -c1-12; else echo "(missing)"; fi; }
short() { printf '%s' "$1" | sed "s|^$HOME|~|"; }
mtime() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null || echo 0; }  # GNU first: GNU stat -f prints filesystem info
# warn when a binary is older than the HEAD commit of the submodule it is built from
stale() { # file submodule-dir
  f=$1; d=$repo/$2; [ -f "$f" ] && [ -e "$d/.git" ] || return 0
  ct=$(git -C "$d" log -1 --format=%ct 2>/dev/null || echo 0)
  [ "$(mtime "$f")" -ge "$ct" ] || echo " STALE: built before $2's HEAD commit (rebuild it)"
}
header() {
  mod=${MODULE:-$(cd "$repo/dscheevos/module/build" && pwd)}
  dirty=; git -C "$repo" diff --quiet HEAD -- 2>/dev/null || dirty=" (+uncommitted changes)"
  echo "gamecost: $1, $2 intervals, skip ${SKIP:-0}, script $([ -n "$3" ] && basename "$3" || echo none)"
  echo "  repo     $(git -C "$repo" rev-parse --short HEAD)$dirty"
  echo "  runner   $(sha "${RUNNER:-}") $(short "${RUNNER:-}")$(stale "${RUNNER:-}" tools/melonDS)"
  echo "           melonDS $(git -C "$repo/tools/melonDS" rev-parse --short HEAD 2>/dev/null || echo ?)"
  echo "  bootstrap $(sha "${BOOTSTRAP:-}") $(short "${BOOTSTRAP:-}")$(stale "${BOOTSTRAP:-}" external/nds-bootstrap)"
  echo "           nds-bootstrap $(git -C "$repo/external/nds-bootstrap" rev-parse --short HEAD 2>/dev/null || echo ?)"
  echo "  module   $(sha "$mod/dscheevos9.bin") $(wc -c < "$mod/dscheevos9.bin" 2>/dev/null | tr -d ' ') bytes $(short "$mod")"
  echo "  loader   $(sha "$repo/dscheevos/launcher/loader/out/load.bin")   launcher $(sha "$repo/dscheevos/launcher/dscheevos-launcher.nds")"
  if [ -n "${SET:-}" ]; then echo "  set      $(basename "$SET") $(sha "$SET")"; else echo "  set      synthetic typical set (ab.sh default; NOT a real set)"; fi
  echo "  runner args  --thumb-cycle ${EXTRA_ARGS:-} (cache model 1); profiling options in EXTRA_ARGS (--profile-pcs, --profile-idle-*) change some games' workload"
}
[ $# -ge 1 ] || { sed -n '2,13p' "$0"; exit 2; }
game=$1 frames=${2:-1500} script=${3:-}
# a port of its own: two lanes' mock servers on ab.sh's default port make the launcher fail
# ("nds-bootstrap didn't use the session"); emu-lock exports one per slot
header "$game" "$frames" "$script"
PORT=${PORT:-$((8500 + $$ % 400))}
. "$here/../lib/workdir.sh"
dir=${OUT:-$(dsc_mkwork "dsc-gc-p$PORT")}
[ -n "${OUT:-}" ] || { dsc_softterm; trap 'dsc_rmwork "$dir"' EXIT; }
mkdir -p "$dir"
PORT=$PORT PROFDIR=$dir EXTRA_ARGS="--thumb-cycle ${EXTRA_ARGS:-}" CACHE_MODEL=1 MODES="off on" \
  "$here/ab.sh" "$game" "$frames" $script > "$dir/ab.txt" 2>&1 || { tail -20 "$dir/ab.txt"; exit 1; }
grep "^FAIL\|^server saw" "$dir/ab.txt" || true
python3 "$here/gamecost.py" ${SKIP:+--skip $SKIP} ${JSON:+--json} "$dir/prof.off.csv" "$dir/prof.on.csv"
