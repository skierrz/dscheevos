#!/bin/sh
# End-to-end: a retail ROM through the DScheevos nds-bootstrap in melonDS (DSi mode).
# Needs files that can't live in the repo or in CI:
#   DSI_FILES  dir with biosnds7/9.bin, biosdsi7/9.bin, dsifirmware.bin, dsinand.bin (your own dumps)
#   ROM        your own dump of the game
#   BOOTSTRAP  nds-bootstrap-nightly.nds built from skierrz/nds-bootstrap (dscheevos branch)
#   RUNNER     melonds-headless from skierrz/melonDS (dscheevos branch)
# Optional: SCRIPT=file (timed input), WORK=dir, SESSION_ARGS / RUNNER_ARGS (extra mksession / runner options),
#   SD_SETUP="command" (run with the SD card's directory as its last argument before the emulator starts).
# Usage: run.sh HASH SET.json FRAMES [status.py checks, e.g. --expect 9901 --never 9902]
set -eu
: "${DSI_FILES:?}" "${ROM:?}" "${BOOTSTRAP:?}" "${RUNNER:?}"
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
hash=$1 set=$2 frames=$3; shift 3
work=${WORK:-$(mktemp -d)}
sd=$work/sd
# no WORK: delete the files on exit, with the runner's SD image (<sd>.img, .img.idx)
if [ -z "${WORK:-}" ]; then
  trap 'rm -rf "$work" || true; rm -f "$work.img" "$work.img.idx" || true' EXIT
fi
mkdir -p "$sd/_nds/dscheevos" "$sd/games"
cp "$ROM" "$sd/games/game.nds"
[ -f "$sd/games/game.sav" ] || head -c 524288 /dev/zero | tr '\0' '\377' > "$sd/games/game.sav"
cp "$BOOTSTRAP" "$sd/_nds/nds-bootstrap-nightly.nds"
cp "$repo/dscheevos/module/build/dscheevos9.bin" "$sd/_nds/dscheevos/"
printf '[NDS-BOOTSTRAP]\nNDS_PATH = sd:/games/game.nds\nSAV_PATH = sd:/games/game.sav\nLOGGING = 1\nLOADING_SCREEN = 0\n' > "$sd/_nds/nds-bootstrap.ini"
python3 "$repo/tools/mksession/mksession.py" --rom-path sd:/games/game.nds --hash "$hash" --set "$set" ${SESSION_ARGS:-} -o "$sd/_nds/dscheevos/session.bin"
cp "$DSI_FILES/dsinand.bin" "$work/nand.bin"
[ -z "${SD_SETUP:-}" ] || $SD_SETUP "$sd"
"$RUNNER" --mode dsi --rom "$BOOTSTRAP" --arg sd:/_nds/nds-bootstrap-nightly.nds \
  --bios-dir "$DSI_FILES" --nand "$work/nand.bin" --sd "$sd" --frames "$frames" \
  ${SCRIPT:+--script "$SCRIPT"} ${RUNNER_ARGS:-} --screenshot "$work/screen.ppm" --dump-ram "$work/ram.bin" 2>&1 | tail -1
grep -a "DScheevos" "$sd/NDSBTSRP.LOG" | tr -cd '[:print:]\n' || true
python3 "$here/status.py" "$work/ram.bin" "$repo/dscheevos/module/build/dscheevos9.map" "$@"
