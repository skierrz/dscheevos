#!/bin/sh
# End-to-end in melonDS (DSi mode, Wi-Fi through libslirp), started the way a
# user starts a game: TWiLight Menu++ -> pick the game (A) -> DScheevos launcher
# (installed as _nds/nds-bootstrap-release.nds) -> HTTPS session on the mock
# server -> chainload -> DScheevos nds-bootstrap -> game, then the module's
# status from a RAM dump.
#
# TWiLight Menu++ is downloaded by get-twilight.sh (pinned release, SHA-256
# checked, cached); it is never kept in the repo.
# Needs files that can't live in the repo or in CI:
#   DSI_FILES  dir with biosnds7/9.bin, biosdsi7/9.bin, dsifirmware.bin, dsinand.bin (your own dumps)
#   ROM        your own dump of the game
#   BOOTSTRAP  nds-bootstrap-nightly.nds built from skierrz/nds-bootstrap (dscheevos branch)
#   RUNNER     melonds-headless from skierrz/melonDS (dscheevos branch)
# Optional:
#   BOOT       direct (default): the runner boots sd:/BOOT.NDS directly, like Unlaunch does.
#              unlaunch: cold boot through the DSi boot ROM and the NAND in UNLAUNCH_NAND, a copy
#              with Unlaunch installed and its NO BUTTON option set to sdmc:/BOOT.NDS (see README.md)
#   WORK       dir to keep the SD folder, logs, screenshots and RAM dump (default: a private dir under DSC_WORK, deleted on exit)
#   PORT       mock server port (default 8443)
#   HASH       only serve the set for this ROM hash (checks the launcher hashed the game TWiLight picked)
#   PRESS_A    frame at which A is pressed in the TWiLight menu (default 1100)
#   SHOTS      frames to screenshot, e.g. "900 1500 2400"; each is a separate run from a fresh SD
#   TWL_VERSION, TWL_SHA256, TWL_CACHE  see get-twilight.sh
# Usage: run.sh SET.json FRAMES [status.py checks, e.g. --expect 9901 --never 9902]
set -eu
: "${DSI_FILES:?}" "${ROM:?}" "${BOOTSTRAP:?}" "${RUNNER:?}"
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
set=$1 frames=$2; shift 2
port=${PORT:-8443}
. "$here/../lib/workdir.sh"
work=${WORK:-$(dsc_mkwork "dsc-tw-p$port")}
[ -n "${WORK:-}" ] || tmpwork=$work   # no WORK: delete the files on exit
press_a=${PRESS_A:-1100}
case ${BOOT:-direct} in
  direct) nand=$DSI_FILES/dsinand.bin boot_args="--arg sd:/BOOT.NDS" ;;
  unlaunch) nand=${UNLAUNCH_NAND:?} boot_args=--firmware-boot ;;
  *) echo "BOOT must be direct or unlaunch" >&2; exit 2 ;;
esac
rom_name=$(basename "$ROM")
mkdir -p "$work"
work=$(cd "$work" && pwd)

# --- TWiLight Menu++ (DSi package: BOOT.NDS for Unlaunch, _nds/TWiLightMenu, roms/)
"$here/get-twilight.sh" "$work/twl"

# --- throwaway CA and server certificate for the mock server
mkdir -p "$work/tls"
openssl ecparam -name prime256v1 -genkey -noout -out "$work/tls/ca.key"
openssl req -x509 -new -key "$work/tls/ca.key" -days 30 -subj "/CN=DScheevos Test CA" -out "$work/tls/ca.pem"
openssl ecparam -name prime256v1 -genkey -noout -out "$work/tls/server.key"
openssl req -new -key "$work/tls/server.key" -subj "/CN=10.64.0.1" -out "$work/tls/server.csr"
printf 'subjectAltName=IP:10.64.0.1\nbasicConstraints=CA:FALSE\n' > "$work/tls/ext.cnf"
openssl x509 -req -in "$work/tls/server.csr" -CA "$work/tls/ca.pem" -CAkey "$work/tls/ca.key" -CAcreateserial \
  -days 30 -extfile "$work/tls/ext.cnf" -out "$work/tls/server.pem" 2>/dev/null

# --- SD card as installed by a user: TWiLight Menu++, then DScheevos over it
sd0=$work/sd-fresh
rm -rf "$sd0" && mkdir -p "$sd0"
cp -r "$work/twl/BOOT.NDS" "$work/twl/_nds" "$work/twl/roms" "$sd0/"
cp "$ROM" "$sd0/roms/nds/$rom_name"
# Settings TWiLight would ask for on first boot (language, region), no splash
# or logo, DSi theme, games from roms/nds, nds-bootstrap "release" (= the launcher).
# melonDS's SD image doesn't use 32 KB clusters; skip TWiLight's warning about it
# (what "X: Don't show again" saves).
cat > "$sd0/_nds/TWiLightMenu/settings.ini" <<EOF
[SRLOADER]
LANGUAGE_SET = 1
REGION_SET = 1
DONT_SHOW_CLUSTER_WARNING = 1
THEME = 0
DSI_SPLASH = 0
SHOWLOGO = 0
SHOW_DIRECTORIES = 0
SHOW_BOX_ART = 0
ROM_FOLDER = sd:/roms/nds
BOOTSTRAP_FILE = 0
EOF
mkdir -p "$sd0/_nds/dscheevos"
printf '[DSCHEEVOS]\nENABLED = 1\nHARDCORE = 1\nUSERNAME = tester\nTOKEN = MockToken\nHOST = https://10.64.0.1:%s\n' "$port" > "$sd0/_nds/dscheevos/dscheevos.ini"
# LAB (strip-lab): NOTICE_EDGE=auto|top|bottom, where on-screen notices go
[ -z "${NOTICE_EDGE:-}" ] || printf 'NOTICE_EDGE = %s\n' "$NOTICE_EDGE" >> "$sd0/_nds/dscheevos/dscheevos.ini"
# LAB: more lines for dscheevos.ini, e.g. "BADGE_SIZE = 32"
[ -z "${EXTRA_INI:-}" ] || printf '%b\n' "$EXTRA_INI" >> "$sd0/_nds/dscheevos/dscheevos.ini"
cp "$repo/dscheevos/launcher/dscheevos-launcher.nds" "$sd0/_nds/nds-bootstrap-release.nds"
cp "$repo/dscheevos/launcher/loader/out/load.bin" "$repo/dscheevos/module/build/dscheevos9.bin" "$work/tls/ca.pem" "$sd0/_nds/dscheevos/"
cp "$BOOTSTRAP" "$sd0/_nds/dscheevos/nds-bootstrap.nds"

# Pick the game: it is the only one in roms/nds, so the cursor is already on it.
printf '# TWiLight Menu++ DSi theme: A on the selected game\n%s 0x1 6\n' "$press_a" > "$work/keys.txt"
# LAB (strip-lab): extra script lines, e.g. "3400 SHOT"
[ -z "${EXTRA_SCRIPT:-}" ] || printf '%b\n' "$EXTRA_SCRIPT" >> "$work/keys.txt"

topng() { python3 -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$1" "$2" 2>/dev/null && rm -f "$1"; }

# One emulator run from a fresh copy of the SD, with its own mock server.
mock=
dsc_softterm   # so the EXIT trap below also runs when the run is killed
trap '[ -z "$mock" ] || kill $mock 2>/dev/null; [ -z "${tmpwork:-}" ] || dsc_rmwork "$tmpwork"' EXIT
run() { # run NAME FRAMES
  d=$work/$1
  rm -rf "$d" && mkdir -p "$d" && cp -r "$sd0" "$d/sd"
  cp "$nand" "$d/nand.bin"
  python3 "$repo/tools/mock-ra-server/server.py" --bind 0.0.0.0 --port "$port" --set "$set" --log "$d/events.jsonl" \
    ${HASH:+--hash "$HASH"} --tls-cert "$work/tls/server.pem" --tls-key "$work/tls/server.key" ${MOCK_ARGS:-} > "$d/mock.log" 2>&1 &
  mock=$!
  sleep 1
  # (the runner needs --rom, which also goes in slot 1; neither boot path starts it from there)
  "$RUNNER" $(dsc_runner_flags) ${RUNNER_ARGS:-} --mode dsi --rom "$d/sd/BOOT.NDS" $boot_args \
    --bios-dir "$DSI_FILES" --nand "$d/nand.bin" --sd "$d/sd" --net --frames "$2" --script "$work/keys.txt" \
    --screenshot "$d/screen.ppm" --dump-ram "$d/ram.bin" > "$d/runner.log" 2>&1 || true
  kill $mock 2>/dev/null; wait $mock 2>/dev/null || true; mock=
  rm -f "$d/nand.bin" "$d/sd.img"
  topng "$d/screen.ppm" "$d/screen.png" || true
  grep '^\[runner\]' "$d/runner.log" || tail -1 "$d/runner.log"
}

for f in ${SHOTS:-}; do
  run "shot-$f" "$f" > /dev/null
  echo "screenshot at frame $f: $(ls "$work/shot-$f"/screen.*)"
done

run main "$frames"
d=$work/main
# TWiLight writes the ini with CRLF line ends
tr -d '\r' < "$d/sd/_nds/nds-bootstrap.ini" > "$d/nds-bootstrap.ini" 2>/dev/null || true
echo "nds-bootstrap.ini written by TWiLight Menu++:"; grep -E '^(NDS_PATH|SAV_PATH|DSI_MODE|QUIT_PATH) ' "$d/nds-bootstrap.ini" || true
echo "server saw:"; cat "$d/events.jsonl" 2>/dev/null || true
fails=
grep -qx "NDS_PATH = sd:/roms/nds/$rom_name" "$d/nds-bootstrap.ini" 2>/dev/null || fails="$fails; TWiLight didn't write NDS_PATH for the game"
grep -q '"startsession"' "$d/events.jsonl" 2>/dev/null || fails="$fails; no session started"
[ -f "$d/sd/_nds/dscheevos/session.used" ] || fails="$fails; nds-bootstrap didn't use the session"
if [ -n "$fails" ]; then echo "RESULT FAIL: ${fails#; }"; exit 1; fi
python3 "$repo/tests/bootstrap/status.py" "$d/ram.bin" "$repo/dscheevos/module/build/dscheevos9.map" "$@"
