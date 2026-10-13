#!/bin/sh
# measure.sh WORK ID FRAMES [TRACE]: conditions evaluated per frame, stock vs change-driven (stats builds)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$1 id=$2 frames=$3
set=$(ls $HOME/Developer/DScheevos/private/sets/sets-$id-*.json | head -1)
base=$HOME/Developer/DScheevos/private/ramdumps/realsets/$id.bin
mkdir -p "$work"
[ -f "$work/session-none.json" ] || python3 "$here/gen.py" session "$set" none 1 > "$work/session-none.json"
trace=${4:-$work/fuzz-$frames.dtrc}
[ -f "$trace" ] || python3 "$here/gen.py" fuzz "$set" "$base" 12345 "$frames" > "$trace"
common="--set $set --session $work/session-none.json --trace $trace --hardcore 1"
"$here/build/difftest-stats-stock" $common --out /dev/null --stats "$work/st-stock.txt"
"${PBIN:-$here/build/difftest-stats-patched}" $common --features changedriven --out /dev/null --stats "$work/st-cd.txt"
python3 "$here/summ.py" "$work/st-stock.txt" "$work/st-cd.txt" "$id" "$trace"
