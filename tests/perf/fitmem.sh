#!/bin/bash
# Does each set fit the module's memory while it loads? (tests/perf/fitmem.c)
#
#   tests/perf/fitmem.sh <dir with sets-<game>-<hash>.json> [game id...]
#
# Optional: FRAMES (default 300), LOW (low heap piece bytes: DSCM_SESSION -
#   __heap_start of the module's map; default: read from build/dscheevos9.map
#   if present), HIGH (high piece bytes; default the DS's; 0: unlimited, to see
#   what a load needs), DSC_FLAGS (the module's evaluation flags; default
#   the module's). Output: one RESULT line per set (plus STEP lines).
# Sets are the server's achievementsets replies; they are not in the repo.
set -eo pipefail
cd "$(dirname "$0")/../.."
[ $# -ge 1 ] || { sed -n '2,11p' "$0"; exit 2; }
SETS=$(cd "$1" && pwd); shift
IDS="$*"
[ -n "$IDS" ] || IDS=$(ls "$SETS" | sed -n 's/^sets-\([0-9]*\)-.*\.json$/\1/p' | sort -n | tr '\n' ' ')
OUT=${OUT:-build/fitmem}
QIMAGE=dscheevos-qplug:8.2.2
MAP=dscheevos/module/build/dscheevos9.map
if [ -z "$LOW" ] && [ -f $MAP ]; then
  hs=$(sed -n 's/^ *0x\([0-9a-f]*\) *__heap_start = .*/\1/p' $MAP)
  [ -n "$hs" ] && LOW=$(( 0x02E00000 - ((0x${hs} & 0x00FFFFFF | 0x02000000) + 7 & ~7) ))
fi
LOW=${LOW:-85784}
docker image inspect $QIMAGE >/dev/null 2>&1 || docker build -t $QIMAGE tests/perf/qplug
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 bash -c '
  set -e; export PATH=$DEVKITARM/bin:$PATH
  make -s -C dscheevos/module -f Makefile -f ../../tests/perf/fitmem.mk BUILD=/src/'$OUT' '"${DSC_FLAGS+DSC_FLAGS=\"$DSC_FLAGS\"}"' /src/'$OUT'/fitmem.elf'
for id in $IDS; do
  f=$(cd "$SETS" && ls sets-$id-*.json | head -1)
  echo "== $id ($f)"
  docker run --rm -v "$PWD/$OUT:/b:ro" -v "$SETS:/sets:ro" $QIMAGE \
    qemu-arm-plug -cpu arm946 /b/fitmem.elf /sets/$f ${FRAMES:-300} $LOW ${HIGH-}
done
