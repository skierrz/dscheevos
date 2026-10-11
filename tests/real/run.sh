#!/bin/sh
# The launcher flow against the real retroachievements.org, in melonDS (DSi
# mode, Wi-Fi through libslirp): launcher -> HTTPS session -> chainload ->
# DScheevos nds-bootstrap -> game, then the module's status from a RAM dump.
#
#   run.sh --check
#     No account needed: runs only the launcher with a made-up user and token.
#     Passes if the server answered login2 over TLS with its invalid-credentials
#     reply (HTTP 401), as logged in _nds/dscheevos/launcher.log. Any other
#     status (Cloudflare 403, maintenance 5xx) fails as "not a login answer".
#     Needs DSI_FILES and RUNNER.
#
#   RA_USER=name RA_PASSWORD=... run.sh [status.py checks, e.g. --expect 12345]
#     (or RA_TOKEN instead of RA_PASSWORD). Logs in from this machine with curl
#     to get the token. Spectator mode by default: nothing is sent to your
#     account. LIVE_UNLOCKS=1 sends unlocks for real.
#     Needs DSI_FILES, ROM, BOOTSTRAP, RUNNER (see tests/launcher/run.sh).
#
# Optional: HARDCORE (default 1), FRAMES (default 6000; 2400 for --check),
#   SCRIPT=file (timed input), WORK=dir, RUNNER_ARGS.
# The work dir holds the token in its dscheevos.ini: delete it afterwards.
set -eu
: "${DSI_FILES:?}" "${RUNNER:?}"
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
check=0
if [ "${1:-}" = "--check" ]; then check=1; shift; fi
work=${WORK:-$(mktemp -d)}
sd=$work/sd
mkdir -p "$sd/_nds/dscheevos" "$sd/games"

if [ $check = 1 ]; then
  frames=${FRAMES:-2400}
  # any valid ROM hashes; the launcher itself will do
  cp "$repo/dscheevos/launcher/dscheevos-launcher.nds" "$sd/games/game.nds"
  user=dscheevos-check token=0000000000000000 spectator=1
else
  : "${ROM:?}" "${BOOTSTRAP:?}" "${RA_USER:?}"
  frames=${FRAMES:-6000}
  cp "$ROM" "$sd/games/game.nds"
  user=$RA_USER
  if [ -n "${RA_TOKEN:-}" ]; then
    token=$RA_TOKEN
  else
    : "${RA_PASSWORD:?set RA_PASSWORD or RA_TOKEN}"
    # password on stdin, not on the command line; the token only goes to the work dir
    reply=$(printf '%s' "$RA_PASSWORD" | curl -sS -A "DScheevos-tests/$(cat "$repo/VERSION")" \
      -d r=login2 --data-urlencode "u=$RA_USER" --data-urlencode p@- https://retroachievements.org/dorequest.php)
    token=$(printf '%s' "$reply" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("Token") or "")')
    [ -n "$token" ] || { echo "login failed: $(printf '%s' "$reply" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("Error"))')"; exit 1; }
  fi
  if [ "${LIVE_UNLOCKS:-0}" = 1 ]; then spectator=0; else spectator=1; fi
  [ -f "$sd/games/game.sav" ] || head -c 524288 /dev/zero | tr '\0' '\377' > "$sd/games/game.sav"
  cp "$repo/dscheevos/launcher/loader/out/load.bin" "$repo/dscheevos/module/build/dscheevos9.bin" "$sd/_nds/dscheevos/"
  cp "$BOOTSTRAP" "$sd/_nds/dscheevos/nds-bootstrap.nds"
fi

printf '[NDS-BOOTSTRAP]\nNDS_PATH = sd:/games/game.nds\nSAV_PATH = sd:/games/game.sav\nLOGGING = 1\nLOADING_SCREEN = 0\n' > "$sd/_nds/nds-bootstrap.ini"
# no HOST: the real server
( umask 077
  printf '[DSCHEEVOS]\nENABLED = 1\nHARDCORE = %s\nSEND_DURING_PLAY = 1\nUSERNAME = %s\nTOKEN = %s\nSPECTATOR = %s\n' \
    "${HARDCORE:-1}" "$user" "$token" "$spectator" > "$sd/_nds/dscheevos/dscheevos.ini" )
cp "$repo/dscheevos/launcher/dscheevos-launcher.nds" "$sd/_nds/nds-bootstrap-release.nds"
cp "$DSI_FILES/dsinand.bin" "$work/nand.bin"
[ $spectator = 1 ] && echo "spectator mode: nothing is sent" || echo "LIVE_UNLOCKS=1: unlocks are sent to $user's account"

"$RUNNER" --mode dsi --rom "$sd/_nds/nds-bootstrap-release.nds" --arg sd:/_nds/nds-bootstrap-release.nds \
  --bios-dir "$DSI_FILES" --nand "$work/nand.bin" --sd "$sd" --net --frames "$frames" ${SCRIPT:+--script "$SCRIPT"} ${RUNNER_ARGS:-} \
  --screenshot "$work/screen.ppm" --dump-ram "$work/ram.bin" > "$work/runner.log" 2>&1 || true
log=$sd/_nds/dscheevos/launcher.log
echo "launcher.log:"; tail -n 20 "$log" 2>/dev/null || echo "(none)"
echo "work dir: $work (holds the token: delete it when done)"

if [ $check = 1 ]; then
  # the real server refuses a bad token with HTTP 401 (invalid_credentials)
  if grep -q '^login2: HTTP 401' "$log" 2>/dev/null; then
    echo "RESULT PASS: retroachievements.org answered over TLS (credentials refused)"
    exit 0
  fi
  status=$(sed -n 's/^login2: HTTP \([0-9]*\).*/\1/p' "$log" 2>/dev/null | tail -n 1)
  if [ -n "$status" ]; then
    echo "RESULT FAIL: reached the host over TLS, but HTTP $status is not a login answer"
  else
    echo "RESULT FAIL: no answer from retroachievements.org"
  fi
  exit 1
fi
echo "runner.log:"; grep -v "^\[pc\]" "$work/runner.log" | tail -n 5
[ -f "$sd/_nds/dscheevos/session.used" ] || { echo "RESULT FAIL: nds-bootstrap didn't use the session"; exit 1; }
python3 "$repo/tests/bootstrap/status.py" "$work/ram.bin" "$repo/dscheevos/module/build/dscheevos9.map" "$@"
