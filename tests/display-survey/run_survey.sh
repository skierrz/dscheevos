#!/bin/sh
# M1 display survey driver: one title per emu-lock hold, plain DS mode, 15 min
# of emulated time, generic input, screenshots at 5/10/15 min.
# Usage: run_survey.sh OUTDIR "ROM name.nds"...   (skips titles already done)
# Env: SURVEY_RUNNER (default ../melonDS-survey/build-survey/melonds-headless),
#      FRAMES (default 54000), ROMDIR (required: the .nds dumps; read-only).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)              # ~/Developer/DScheevos
. "${DSC_ENV:-$top/dscheevos.env}"   # needs DSI_FILES (biosnds7/9, firmware)
runner=${SURVEY_RUNNER:-$top/melonDS-survey/build-survey/melonds-headless}
frames=${FRAMES:-54000}
romdir=${ROMDIR:?set ROMDIR to the directory of .nds dumps}
out=$1; shift
mkdir -p "$out"
[ -f "$out/input.txt" ] || python3 "$here/make_script.py" "$frames" ${TOUCHY:+--touchy} > "$out/input.txt"
for rom in "$@"; do
  slug=$(printf '%s' "${rom%.nds}" | tr -c 'A-Za-z0-9\n' '_' | cut -c1-60)
  [ -f "$out/$slug.json" ] && { echo "skip $slug"; continue; }
  echo "== $rom ($(date +%H:%M))"
  t1=$((frames / 3)); t2=$((2 * frames / 3))
  "$top/bin/emu-lock" -l display-survey "$runner" --rom "$romdir/$rom" --bios-dir "$DSI_FILES" \
    --frames "$frames" --script "$out/input.txt" --survey "$out/$slug" \
    --shot-at "$((t1 - 1)):$out/$slug.5min.ppm" --shot-at "$((t2 - 1)):$out/$slug.10min.ppm" \
    --screenshot "$out/$slug.15min.ppm" > "$out/$slug.log" 2>&1 || echo "runner exit $? for $slug"
  for p in "$out/$slug".*.ppm; do
    [ -f "$p" ] && sips -s format png "$p" --out "${p%.ppm}.png" > /dev/null && rm -f "$p"
  done
  gzip -f "$out/$slug.frames.csv" 2>/dev/null || true
  printf '%s\t%s\n' "$slug" "$rom" >> "$out/titles.tsv"
done
