#!/bin/sh
# In-game menu pause: VBlanks while nds-bootstrap's in-game menu is open are
# no gap (the game is stopped, as in an emulator's pause menu), so they must
# add to neither frames_dropped nor frames_missed (docs/dropped-frames.md).
# Two runs of the same game: no menu, then the menu open for ~10 s. Same
# missed count, nothing dropped, and the menu run counts ~600 fewer frames
# (proof the menu was open and the game stopped).
# Needs what run.sh needs. Usage: menupause.sh [SET.json] (default: live.json)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
set=${1:-$here/../bootstrap/sets/live.json}
open=${MENU_AT:-3000} hold=${MENU_FRAMES:-600} frames=$((${MENU_AT:-3000} + ${MENU_FRAMES:-600} + 1500))
s=$(mktemp); trap 'rm -f "$s"' EXIT
printf '%d 0x284 20\n%d 0x2 5\n' "$open" $((open + hold)) > "$s"   # L+Down+Select opens, B closes
counts() { sed -n "s/^{'active'.*'snapshotted': \([0-9]*\).*'dropped': \([0-9]*\), 'missed': \([0-9]*\).*/\1 \2 \3/p" "$1"; }
a=$(mktemp); b=$(mktemp); trap 'rm -f "$s" "$a" "$b"' EXIT
"$here/run.sh" "$set" "$frames" > "$a" 2>&1 || true
SCRIPT=$s "$here/run.sh" "$set" "$frames" > "$b" 2>&1 || true
set -- $(counts "$a") $(counts "$b")
[ $# -eq 6 ] || { echo "RESULT FAIL: no status (see run output)"; tail -5 "$a" "$b"; exit 1; }
echo "no menu:   snapshotted $1 dropped $2 missed $3"
echo "menu open: snapshotted $4 dropped $5 missed $6"
fails=""
[ "$2" = 0 ] && [ "$5" = 0 ] || fails="$fails frames dropped;"
[ "$3" = "$6" ] || fails="$fails missed $3 without the menu, $6 with it;"
[ $(($1 - $4)) -ge $((hold * 5 / 6)) ] || fails="$fails the menu run counted only $(($1 - $4)) fewer frames (menu not open?);"
[ -z "$fails" ] && echo "RESULT PASS" || { echo "RESULT FAIL:$fails"; exit 1; }
