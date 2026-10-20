#!/bin/sh
# Behaviour gate: one emulator run (melonDS, cache model, --thumb-cycle, DSi mode via tests/perf/ab.sh). See README.md.
#   run.sh save GAME STATE                 achievements-on run from boot (real set, census input) that saves STATE at the
#                                          game's script frame (table below), i.e. in play
#   run.sh load GAME STATE BASE [args...]  from STATE, $FRAMES (3000) VBlank intervals with the same input; writes
#                                          BASE.bg.{meta,snap,frames.csv,io.csv}, BASE.spu7, BASE.pcm, BASE.prof.csv, BASE.log
# Reference:  run.sh load GAME STATE BASE --dsc-neutral --dsc-nonet   (nds-bootstrap without DScheevos, same state)
# Under test: run.sh load GAME STATE BASE --dsc-nonet                 (DScheevos as built)
# Then: bgate.py REFBASE BASE. Every run goes through emu-lock and hardtimeout.
# Env: RUNNER (with --bgate), BOOTSTRAP, MODULE, TREE (the build under test: nds-bootstrap, module, launcher tree), LIB (ROM folder), DSC_PRIVATE (sets,
#      census scripts), FRAMES (3000), TMO (seconds, 3000). Logs are redacted (long tokens); dumps are never printed.
set -u
here=$(cd "$(dirname "$0")" && pwd); repo=$here/../..
kind=$1 game=$2 state=$3; shift 3
P=${DSC_PRIVATE:?} L=${LIB:?}
case $game in
  kirby)    rom="Kirby Super Star Ultra (USA).nds"; id=14763; sc=kirby; s=1000;;
  mkds)     rom="Mario Kart DS (USA, Australia) (En,Fr,De,Es,It).nds"; id=12711; sc=mkds; s=2000;;
  black)    rom="Pokemon - Black Version (USA, Europe) (NDSi Enhanced).nds"; id=3887; sc=black; s=8000;;
  platinum) rom="Pokemon - Platinum Version (USA) (Rev 1).nds"; id=11732; sc=platinum; s=1000;;
  tetris)   rom="Tetris DS (USA, Australia).nds"; id=9878; sc=tetris; s=1500;;
  layton)   rom="Professor Layton and the Curious Village (USA, Australia).nds"; id=9518; sc=layton; s=3500;;
  phoenix)  rom="Phoenix Wright - Ace Attorney (USA).nds"; id=12747; sc=phoenix; s=750;;
  digimon)  rom="Digimon World DS (USA).nds"; id=16153; sc=digimon; s=5500;;
  starfy)   rom="Legendary Starfy, The (USA).nds"; id=9119; sc=generic; s=1500;;   # IRQ-heavy (~420 IRQs a frame); menus only (generic script)
  *) echo "unknown game $game"; exit 2;;
esac
rom=$L/$rom; [ -f "$rom" ] || { echo "no ROM $rom"; exit 2; }
export SET=$(ls $P/sets/sets-$id-*.json | head -1)
script=$P/census/scripts/$sc.txt
case $kind in
  save) fr=$((s + 10)); extra="--save-state-at -$s:$state"; out=$state.log;;
  load) base=$1; shift
        [ -f "$state" ] || { echo "no state $state"; exit 1; }
        fr=${FRAMES:-3000}; out=$base.log
        extra="--load-state $state --bgate $base.bg --hash9 $base.hash --audio-out $base.pcm --spu7 $base.spu7";;
  *) echo "kind: save or load"; exit 2;;
esac
h() { { sha1sum "$1" 2>/dev/null || shasum "$1"; } | cut -c1-12; }
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
{ echo "bgate $kind $game: $(git -C "$repo" rev-parse --short HEAD) args: $*"
  echo "runner $(h "$RUNNER") bootstrap $(h "$BOOTSTRAP") module $(h "${MODULE:-$repo/dscheevos/module/build}/dscheevos9.bin") set $(basename "$SET")"; } > "$out"
# TREE: the checkout whose launcher/loader build ab.sh uses (default this one; e.g. the build under test's tree)
T=${TREE:-$repo}
MODES=on CACHE_MODEL=1 PROFDIR=$D EXTRA_ARGS="--thumb-cycle $extra $*" \
  emu-lock -l bgate-$game "$T/tests/perf/hardtimeout" ${TMO:-3000} "$T/tests/perf/ab.sh" "$rom" $fr "$script" 2>&1 \
  | sed -E 's/[A-Za-z0-9+\/=_-]{24,}/<redacted>/g' >> "$out"
echo "rc=$?" >> "$out"
[ $kind = load ] && mv "$D/prof.on.csv" "$base.prof.csv" 2>/dev/null
tail -2 "$out"
