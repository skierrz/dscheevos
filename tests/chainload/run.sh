#!/bin/bash
# Runs the chainload tests in the headless melonDS runner (DSi mode).
#   MELONDS_HEADLESS=path/to/melonds-headless
#   DSI_FILES=dir with biosdsi7/9.bin, biosnds7/9.bin, dsfirmware.bin, dsifirmware.bin, dsinand.bin
#   LOAD_BIN=path/to/load.bin    (default: ../../dscheevos/launcher/loader/out/load.bin)
# Optional second test, chainloading nds-bootstrap the way the launcher will:
#   BOOTSTRAP_NDS=path/to/nds-bootstrap-nightly.nds
#   BOOTSTRAP_SD=dir with _nds/nds-bootstrap.ini and the game it names (copied, not modified)
# Build first: out/*.nds (make here, BlocksDS) and load.bin (dscheevos/launcher/loader).
set -e
: "${MELONDS_HEADLESS:?set MELONDS_HEADLESS}" "${DSI_FILES:?set DSI_FILES}"
here=$(cd "$(dirname "$0")" && pwd)
LOAD_BIN=${LOAD_BIN:-$here/../../dscheevos/launcher/loader/out/load.bin}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
fail=0

run() { # sd-dir chainer-name frames screenshot args...
  local sd=$1 rom=$2 frames=$3 shot=$4; shift 4
  cp "$DSI_FILES/dsinand.bin" "$work/nand.bin"
  "$MELONDS_HEADLESS" --mode dsi --rom "$sd/$rom.nds" --arg "sd:/$rom.nds" "$@" \
    --bios-dir "$DSI_FILES" --nand "$work/nand.bin" --sd "$sd" \
    --frames "$frames" --until DONE --screenshot "$shot" 2>/dev/null
}
expect() { # output pattern
  if grep -qxF -- "$2" <<<"$1"; then echo "  ok   $2"; else echo "  FAIL $2"; fail=1; fi
}

sd=$work/sd1
mkdir -p "$sd/_nds/dscheevos"
cp "$here/out/chainer.nds" "$here/out/chainer-wifi.nds" "$here/out/target.nds" "$sd/"
cp "$LOAD_BIN" "$sd/_nds/dscheevos/load.bin"
echo "chainload SD read works" > "$sd/chainload-test.txt"
for rom in chainer chainer-wifi; do
  echo "== $rom -> target"
  out=$(run "$sd" $rom 1200 "$work/target.ppm")
  echo "$out" | sed 's/^/  | /'
  expect "$out" "TARGET argc=2"
  expect "$out" "TARGET argv[0]=sd:/target.nds"
  expect "$out" "TARGET argv[1]=hello"
  expect "$out" "TARGET fat=1 sd_read=ok text=chainload SD read works"
  grep -q "^TARGET dsi=1 " <<<"$out" && echo "  ok   TARGET dsi=1" || { echo "  FAIL TARGET dsi=1"; fail=1; }
done

echo "== missing target: chainload() returns an error"
out=$(run "$sd" chainer 600 "$work/missing.ppm" --arg sd:/missing.nds)
expect "$out" "CHAINER chainload failed: -2"

if [ -n "$BOOTSTRAP_NDS" ] && [ -n "$BOOTSTRAP_SD" ]; then
  echo "== chainer-wifi -> nds-bootstrap -> game"
  sd=$work/sd2
  cp -a "$BOOTSTRAP_SD" "$sd"
  rm -f "$sd/NDSBTSRP.LOG"
  mkdir -p "$sd/_nds/dscheevos"
  cp "$LOAD_BIN" "$sd/_nds/dscheevos/load.bin"
  cp "$BOOTSTRAP_NDS" "$sd/_nds/dscheevos/nds-bootstrap.nds"
  cp "$here/out/chainer-wifi.nds" "$sd/"
  run "$sd" chainer-wifi 1800 "${SCREENSHOT:-$work/bootstrap.ppm}" --arg sd:/_nds/dscheevos/nds-bootstrap.nds | sed 's/^/  | /'
  if grep -q "^Card patch successful" "$sd/NDSBTSRP.LOG" 2>/dev/null; then
    echo "  ok   NDSBTSRP.LOG: Card patch successful"
  else
    echo "  FAIL NDSBTSRP.LOG missing or game not patched"; fail=1
  fi
  [ -n "$SCREENSHOT" ] && echo "  screenshot: $SCREENSHOT"
fi

[ $fail = 0 ] && echo PASS || { echo FAIL; exit 1; }
