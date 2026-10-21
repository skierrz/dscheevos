#!/bin/bash
# LAB ONLY: strip build vs base build on a list of games (one ROM path per line in LIST), screenshots
# every 60 frames from 3300; one line per game and build in OUT/summary.tsv.
#   tests/strip-lab/sweep.sh LIST BASE_DIR OUT [FRAMES]
# BUILDS (default "strip base"): which builds; other names run dscheevos/module/build-<name>.
here=$(cd "$(dirname "$0")" && pwd); repo=$here/../..
list=$1 base=$2 out=$3 frames=${4:-4600}
mkdir -p "$out"
while IFS= read -r rom; do
  [ -z "$rom" ] && continue
  name=$(basename "$rom" .nds | cut -c1-40 | tr ' ' '_')
  for b in ${BUILDS:-strip base}; do
    case $b in base) src=$base ;; strip) src=$repo/dscheevos/module/build-strip ;; *) src=$repo/dscheevos/module/build-$b ;; esac
    w=$out/$name.$b
    from=3300; [ $b = base ] && from=$((frames - 100))
    "$here/shots.sh" "$src" "$rom" "$frames" $from $((frames - 100)) 60 "$w" > "$w.txt" 2>&1
    lag=$(grep -o 'lag: [0-9]* of [0-9]*' "$w/main/runner.log" 2>/dev/null)
    game=$(grep -o 'outside the module: [0-9]*' "$w/main/runner.log" 2>/dev/null | grep -o '[0-9]*$')
    idle=$(grep -o 'module idle cycles/interval: avg [0-9]*' "$w/main/runner.log" 2>/dev/null | grep -o '[0-9]*$')
    res=$(grep -o 'RESULT [A-Z]*' "$w.txt" | tail -1)
    st=$(grep 'main strip:' "$w.txt" | sed 's/.*strip: //')
    printf '%s\t%s\t%s\t%s\tgame %s\tidle %s\t%s\n' "$name" "$b" "$res" "$lag" "$game" "$idle" "$st" | tee -a "$out/summary.tsv"
    rm -rf "$w/main/sd" "$w/main/ram.bin" "$w/sd-fresh" "$w/twl" "$w/tls"
  done
done < "$list"
