#!/bin/sh
# In-game menu round trip in melonDS (see README.md): the launcher flow of
# tests/launcher/run.sh, then the menu opened (L+DOWN+SELECT) and closed (B)
# twice; checks that the game finds its sub-screen registers, master volume
# and timekeeping as it left them (check.py, one report per round trip).
# Usage: run.sh [GAME]   GAME: kirby (default), pokemon, nsmb or a ROM file
# Optional: OPEN (frame of the first open, default 4500), ENABLED=0 (no
#   DScheevos session: nds-bootstrap's own menu path), TICK=addr (the game's
#   OS tick counter; default: found from RAM dumps in a first, shorter run),
#   KEEP=1 (keep the work folder). Runs the emulator twice; wrap it in emu-lock.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
game=${1:-kirby}
case $game in
  kirby) rom=${ROM_KIRBY:?} ;;
  pokemon) rom=${ROM_POKEMON:?} ;;
  nsmb) rom=${ROM_NSMB:?} ;;
  *) rom=$game ;;
esac
open1=${OPEN:-4500}
close1=$((open1 + 300)) open2=$((open1 + 600)) close2=$((open1 + 1500))
top=$(mktemp -d)
[ "${KEEP:-0}" = 1 ] && echo "work: $top" || trap 'rm -rf "$top"' EXIT
set=$repo/tests/bootstrap/sets/live.json

tick=${TICK:-}
if [ -z "$tick" ]; then
  ROM=$rom WORK=$top/find ENABLED=${ENABLED:-1} RUNNER_ARGS="--dump-ram-at $((open1 - 600)):$top/a.bin \
    --dump-ram-at $((open1 - 60)):$top/b.bin --dump-ram-at $((open1 - 1)):$top/c.bin" \
    "$repo/tests/launcher/run.sh" "$set" "$open1" > "$top/find.out" 2>&1 || true
  tick=$(python3 "$here/find_tick.py" "$top/a.bin" "$top/b.bin" "$top/c.bin" 540 59 | tr '\n' ' ')
  rm -rf "$top/find" "$top"/?.bin
  echo "OS tick counter candidates: ${tick:-none}"
fi

# a game that set these write-only (or once-set) registers and left them: the
# runner writes them 120 frames before each open, then they must survive the menu
pokes=""
for o in $open1 $open2; do
  p=$((o - 120))
  for bg in 0 1 2 3; do
    pokes="$pokes --poke $p:9:$((0x04001010 + bg * 4)):$((0x30 + bg)):16 --poke $p:9:$((0x04001012 + bg * 4)):$((0x50 + bg)):16"
  done
  pokes="$pokes --poke $p:9:0x04001050:0x0C48:16 --poke $p:9:0x04001052:0x0808:16 --poke $p:9:0x04001054:6:16"
  pokes="$pokes --poke $p:9:0x0400104C:0x11:16 --poke $p:7:0x04000500:100:8"
done
printf '%s 0x284 20\n%s 0x2 5\n%s 0x284 20\n%s 0x2 5\n' $open1 $close1 $open2 $close2 > "$top/menu.txt"
words="--trace-word 0x027FFC3C"
targs=""
for t in $tick; do words="$words --trace-word $t"; targs="$targs --tick $t"; done

ROM=$rom WORK=$top/run ENABLED=${ENABLED:-1} SCRIPT=$top/menu.txt RUNNER_ARGS="--trace-io $top/io.csv $words $pokes" \
  "$repo/tests/launcher/run.sh" "$set" $((close2 + 400)) > "$top/run.out" 2>&1 || true
grep -q "^\[poke\]" "$top/run/runner.log" || { echo "RESULT FAIL: no run (see run.out)"; cat "$top/run.out"; exit 1; }
[ "${ENABLED:-1}" = 0 ] || [ -f "$top/run/sd/_nds/dscheevos/session.used" ] || { echo "RESULT FAIL: nds-bootstrap didn't use the session"; exit 1; }
rc=0
echo "== first open"; python3 "$here/check.py" "$top/io.csv" $open1 $close1 $targs || rc=1
echo "== second open"; python3 "$here/check.py" "$top/io.csv" $open2 $close2 $targs || rc=1
[ $rc = 0 ] && echo "RESULT PASS" || echo "RESULT FAIL"
exit $rc
