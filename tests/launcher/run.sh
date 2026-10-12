#!/bin/sh
# End-to-end in melonDS (DSi mode, Wi-Fi through libslirp): launcher -> HTTPS
# session on the mock server -> chainload -> DScheevos nds-bootstrap -> game,
# then the module's status from a RAM dump.
# Needs files that can't live in the repo or in CI:
#   DSI_FILES  dir with biosnds7/9.bin, biosdsi7/9.bin, dsifirmware.bin, dsinand.bin (your own dumps)
#   ROM        your own dump of the game
#   BOOTSTRAP  nds-bootstrap-nightly.nds built from skierrz/nds-bootstrap (dscheevos branch)
#   RUNNER     melonds-headless from skierrz/melonDS (dscheevos branch)
# Optional: SCRIPT=file (timed input), WORK=dir (keep files), DSI_MODE (nds-bootstrap.ini, default 0;
#   DSi-enhanced games get achievements only in DS mode), PORT, LIVE=0 (no sending during play),
#   HARDCORE=0, SPECTATOR=1, MOCK_ARGS (extra mock server options, e.g. --fail-awards),
#   ENABLED=0 (achievements off in dscheevos.ini: the game starts without them),
#   MODULE=dir (dscheevos9.bin and .map from another build),
#   RUNNER_ARGS (extra runner options, e.g. --wpa-ap melonAP:<PMK> for a WPA2 access point).
# Offline queue: run once with MOCK_ARGS=--fail-awards, then again with the same WORK; the
# second launcher run sends the unlocks from _nds/dscheevos/pending.bin (logged with "offset").
# Usage: run.sh SET.json FRAMES [status.py checks, e.g. --expect 9901 --never 9902]
# Wi-Fi drop test: wifidrop.txt drops the access point twice during play (Kirby Super Star
# Ultra's game starts about frame 1800 here); every unlock must still arrive:
#   SCRIPT=tests/launcher/wifidrop.txt tests/launcher/run.sh tests/bootstrap/sets/live.json 6000 \
#     --expect-sent 4 --expect-rejoins 2
set -eu
: "${DSI_FILES:?}" "${ROM:?}" "${BOOTSTRAP:?}" "${RUNNER:?}"
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
set=$1 frames=$2; shift 2
port=${PORT:-8443}
module=${MODULE:-$repo/dscheevos/module/build}
work=${WORK:-$(mktemp -d)}
[ -n "${WORK:-}" ] || tmpwork=$work   # no WORK: delete the files on exit
sd=$work/sd
mkdir -p "$sd/_nds/dscheevos" "$sd/games" "$work/tls"

# throwaway CA and server certificate for the mock server
openssl ecparam -name prime256v1 -genkey -noout -out "$work/tls/ca.key"
openssl req -x509 -new -key "$work/tls/ca.key" -days 30 -subj "/CN=DScheevos Test CA" -out "$work/tls/ca.pem"
openssl ecparam -name prime256v1 -genkey -noout -out "$work/tls/server.key"
openssl req -new -key "$work/tls/server.key" -subj "/CN=10.64.0.1" -out "$work/tls/server.csr"
printf 'subjectAltName=IP:10.64.0.1\nbasicConstraints=CA:FALSE\n' > "$work/tls/ext.cnf"
openssl x509 -req -in "$work/tls/server.csr" -CA "$work/tls/ca.pem" -CAkey "$work/tls/ca.key" -CAcreateserial \
  -days 30 -extfile "$work/tls/ext.cnf" -out "$work/tls/server.pem" 2>/dev/null

cp "$ROM" "$sd/games/game.nds"
[ -f "$sd/games/game.sav" ] || head -c 524288 /dev/zero | tr '\0' '\377' > "$sd/games/game.sav"
printf '[NDS-BOOTSTRAP]\nNDS_PATH = sd:/games/game.nds\nSAV_PATH = sd:/games/game.sav\nLOGGING = 1\nLOADING_SCREEN = 0\nDSI_MODE = %s\n' "${DSI_MODE:-0}" > "$sd/_nds/nds-bootstrap.ini"
printf '[DSCHEEVOS]\nENABLED = %s\nHARDCORE = %s\nSEND_DURING_PLAY = %s\nUSERNAME = tester\nTOKEN = MockToken\nHOST = https://10.64.0.1:%s\nSPECTATOR = %s\n' \
  "${ENABLED:-1}" "${HARDCORE:-1}" "${LIVE:-1}" "$port" "${SPECTATOR:-0}" > "$sd/_nds/dscheevos/dscheevos.ini"
cp "$repo/dscheevos/launcher/dscheevos-launcher.nds" "$sd/_nds/nds-bootstrap-release.nds"
cp "$repo/dscheevos/launcher/loader/out/load.bin" "$module/dscheevos9.bin" "$work/tls/ca.pem" "$sd/_nds/dscheevos/"
cp "$BOOTSTRAP" "$sd/_nds/dscheevos/nds-bootstrap.nds"
cp "$DSI_FILES/dsinand.bin" "$work/nand.bin"

python3 "$repo/tools/mock-ra-server/server.py" --bind 0.0.0.0 --port "$port" --set "$set" --log "$work/events.jsonl" \
  --tls-cert "$work/tls/server.pem" --tls-key "$work/tls/server.key" ${MOCK_ARGS:-} > "$work/mock.log" 2>&1 &
mock=$!
trap 'kill $mock 2>/dev/null; wait $mock 2>/dev/null || true; [ -z "${tmpwork:-}" ] || rm -rf "$tmpwork"' EXIT
sleep 1

"$RUNNER" --mode dsi --rom "$sd/_nds/nds-bootstrap-release.nds" --arg sd:/_nds/nds-bootstrap-release.nds \
  --bios-dir "$DSI_FILES" --nand "$work/nand.bin" --sd "$sd" --net --frames "$frames" ${SCRIPT:+--script "$SCRIPT"} ${RUNNER_ARGS:-} \
  --screenshot "$work/screen.ppm" --dump-ram "$work/ram.bin" > "$work/runner.log" 2>&1 || true
grep -v "^\[pc\]" "$work/runner.log" | tail -1
echo "server saw:"; cat "$work/events.jsonl"
echo "launcher.log:"; cat "$sd/_nds/dscheevos/launcher.log" 2>/dev/null || true
[ -f "$sd/_nds/dscheevos/session.used" ] || { echo "RESULT FAIL: nds-bootstrap didn't use the session"; exit 1; }
python3 "$repo/tests/bootstrap/status.py" "$work/ram.bin" "$module/dscheevos9.map" "$@"
