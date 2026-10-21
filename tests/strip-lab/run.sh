#!/bin/bash
# LAB ONLY: one twilight run of a module build (base or strip), with screenshots and the strip counters.
#   tests/strip-lab/run.sh BUILD_DIR ROM FRAMES "SHOTS" WORK [status.py args]
# BUILD_DIR holds dscheevos9.bin/.map (e.g. dscheevos/module/build-strip). Needs ../dscheevos.env.
set -eu
here=$(cd "$(dirname "$0")" && pwd); repo=$here/../..
src=$1 rom=$2 frames=$3 shots=$4 work=$5; shift 5
mkdir -p "$repo/dscheevos/module/build"
cp "$src/dscheevos9.bin" "$src/dscheevos9.map" "$repo/dscheevos/module/build/"
mkdir -p "$work"
ROM="$rom" WORK="$work" SHOTS="$shots" "$HOME/Developer/DScheevos/bin/emu-lock" -l striplab \
  "$repo/tests/perf/hardtimeout" 1500 "$repo/tests/twilight/run.sh" "$repo/tests/bootstrap/sets/pipeline.json" "$frames" "$@" || true
for f in "$work"/main/screen.ppm.f*.ppm; do
  [ -f "$f" ] && python3 -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$f" "${f%.ppm}.png" && rm -f "$f"
done
grep -E "lag|interval" "$work/main/runner.log" | head -5 || true
for d in "$work"/shot-* "$work"/main; do
  [ -f "$d/ram.bin" ] && { printf '%s ' "$(basename "$d")"; python3 "$here/stats.py" "$d/ram.bin" "$src/dscheevos9.map"; } || true
done
