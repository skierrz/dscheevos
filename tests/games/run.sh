#!/bin/bash
# Run every game test in games.txt.  Needs: DSC_GAMETEST (built dsc-gametest), DSC_ROMS.
set -u
: "${DSC_GAMETEST:?build tests/games and set DSC_GAMETEST}" "${DSC_ROMS:?set DSC_ROMS (see fetch_roms.py)}"
here=$(cd "$(dirname "$0")" && pwd); fail=0
# Each game runs twice: idle time every frame, and idle time only every 600
# frames (10 s), so frames queue up as during a long load. Both must match
# rcheevos reading live memory every frame.
for idle in 1 600; do
while IFS='|' read -r name path hash page frames checks; do
  name=$(echo "$name" | xargs); [[ -z "$name" || "$name" == \#* ]] && continue
  path=$(echo "$path" | xargs); frames=$(echo "$frames" | xargs)
  echo "== $name (idle every $idle)"
  # shellcheck disable=SC2086
  out=$("$DSC_GAMETEST" --rom "$DSC_ROMS/$path" --set "$here/sets/$name.json" --script "$here/scripts/$name.txt" \
       --frames "$frames" --idle-every "$idle" $checks) || fail=1
  echo "$out" | grep -E "^(ok|FAIL|RESULT|reference|stats)"
done < "$here/games.txt"
done
[ $fail = 0 ] && echo "ALL GAME TESTS PASSED" || { echo "SOME GAME TESTS FAILED"; exit 1; }
