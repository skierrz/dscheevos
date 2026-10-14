#!/bin/sh
# Does a game still start and run with achievements on? The same game and input through ab.sh
# (achievements off, then on) and a pass/fail verdict:
#   - both runs reach the game's first OS_Halt (else: the game never started, e.g. a launcher crash);
#   - the on run lags no more than the off run plus max(5, 1% of the intervals) (a hang shows as every
#     interval lagging: Professor Layton and Peggle froze that way until the module's MPU check);
#   - the module evaluated frames, or the launcher refused the set as too big for the module and the
#     game ran without achievements (Animal Crossing: Wild World's 2.4 MB set crashed the launcher).
# Usage: launchcheck.sh ROM [FRAMES] [SCRIPT]       FRAMES default 1500 (intervals from the first OS_Halt)
#        launchcheck.sh --list FILE [FRAMES]        each line: ROM-file-name SET-id SCRIPT-name (see launchcheck.txt);
#                                                   ROMs from LIBRARY (default ~/RomM/library/roms/nds), sets from
#                                                   SETS (default ../private/sets), scripts from SCRIPTS
# Env: SET (with a single ROM), everything ab.sh takes. Wrap it in emu-lock + hardtimeout like any emulator run.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" = --list ]; then
  list=$2 frames=${3:-1500} fails=0
  LIBRARY=${LIBRARY:-$HOME/RomM/library/roms/nds}
  SETS=${SETS:-$HOME/Developer/DScheevos/private/sets}
  SCRIPTS=${SCRIPTS:-$HOME/Developer/DScheevos/private/census/scripts}
  while IFS='|' read -r rom setid script; do
    case $rom in ''|'#'*) continue ;; esac
    set=$(ls "$SETS"/sets-"$setid"-*.json 2>/dev/null | head -1)
    [ -n "$set" ] || { echo "== $rom: FAIL no set $setid in SETS"; fails=$((fails + 1)); continue; }
    SET=$set "$0" "$LIBRARY/$rom" "$frames" "$SCRIPTS/$script" < /dev/null || fails=$((fails + 1))
  done < "$list"
  echo "launchcheck: $fails failed"
  [ "$fails" = 0 ]
  exit
fi
[ $# -ge 1 ] || { sed -n '2,15p' "$0"; exit 2; }
rom=$1 frames=${2:-1500} script=${3:-}
. "$here/../lib/workdir.sh"
out=$(dsc_mkwork "dsc-lc-p${PORT:-8453}")
trap 'exit 130' INT TERM HUP; trap 'dsc_rmwork "$out"' EXIT
"$here/ab.sh" "$rom" "$frames" $script > "$out/ab.txt" 2>&1 || true
python3 - "$out/ab.txt" "$(basename "$rom")" <<'EOF'
import re, sys
text, name = open(sys.argv[1]).read(), sys.argv[2]
modes = {}
for part in re.split(r'^== .*: achievements ', text, flags=re.M)[1:]:
    mode, body = part.split('\n', 1)
    m = re.search(r'^\[prof\] lag: (\d+) of (\d+) intervals', body, re.M)
    e = re.search(r"'evaluated': (\d+)", body)
    modes[mode.strip()] = dict(lag=m and (int(m.group(1)), int(m.group(2))), evaluated=e and int(e.group(1)),
                               no_session='FAIL: nds-bootstrap didn' in body, too_big=re.search(r'over the \d+-byte limit', body) is not None)
off, on = modes.get('off'), modes.get('on')
def verdict():
    if not off or not off['lag']:
        return 'FAIL', 'the achievements-off run never reached the game (no profile): not a module result'
    if not on or not on['lag']:
        return 'FAIL', 'with achievements on the game never started'
    (lo, n), (l1, n1) = off['lag'], on['lag']
    allow = lo + max(5, n // 100)
    lag = 'lag %d / %d of %d / %d intervals' % (lo, l1, n, n1)
    if l1 > allow:
        return 'FAIL', '%s: more than %d with achievements on (a hang lags every interval)' % (lag, allow)
    if on['no_session']:
        if on['too_big']:
            return 'PASS', '%s; the set is too big for the module: the game ran without achievements' % lag
        return 'FAIL', '%s; nds-bootstrap didn\'t use the session' % lag
    if not on['evaluated']:
        return 'FAIL', '%s; the module evaluated no frames' % lag
    return 'PASS', '%s; %d frames evaluated' % (lag, on['evaluated'])
r, why = verdict()
print('== %s: %s, %s' % (name, r, why))
sys.exit(r != 'PASS')
EOF
