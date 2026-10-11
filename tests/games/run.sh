#!/bin/bash
# Run every game test in games.txt.  Needs: DSC_GAMETEST (built dsc-gametest), DSC_ROMS.
set -u
: "${DSC_GAMETEST:?build tests/games and set DSC_GAMETEST}" "${DSC_ROMS:?set DSC_ROMS (see fetch_roms.py)}"
here=$(cd "$(dirname "$0")" && pwd); fail=0
while IFS='|' read -r name path hash page frames checks; do
  name=$(echo "$name" | xargs); [[ -z "$name" || "$name" == \#* ]] && continue
  path=$(echo "$path" | xargs); frames=$(echo "$frames" | xargs)
  echo "== $name"
  # shellcheck disable=SC2086
  out=$("$DSC_GAMETEST" --rom "$DSC_ROMS/$path" --set "$here/sets/$name.json" --script "$here/scripts/$name.txt" \
       --frames "$frames" $checks) || fail=1
  echo "$out" | grep -E "^(ok|FAIL|RESULT|reference|stats)"
done < "$here/games.txt"
[ $fail = 0 ] && echo "ALL GAME TESTS PASSED" || { echo "SOME GAME TESTS FAILED"; exit 1; }
