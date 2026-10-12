#!/bin/bash
# Instruction counts for the DScheevos core under qemu-arm (ARM946): dsc_capture
# (the snapshot, when the game halts), dsc_vblank (queues it, in the game's IPC
# IRQ) and dsc_idle (one frame evaluated),
# on synthetic sets. Builds the core and rcheevos from source every run, with
# the module's own Makefile rules and flags. See tests/perf/README.md.
#
#   tests/perf/run.sh [--gate] [--sets "typical heavy"] [--frames N]
#
# --gate  fail if a count is over its limit in tests/perf/limits.txt
#
# Needs devkitARM ($DEVKITARM), qemu-arm and python3. Without them it runs
# itself in the devkitpro/devkitarm Docker image, with a static qemu-arm from
# $QEMU_ARM (default build/host/qemu-arm, fetched from Ubuntu's
# qemu-user-static on first use).
set -eo pipefail
cd "$(dirname "$0")/../.."
IMAGE=devkitpro/devkitarm:20241104

if [ -z "$DEVKITARM" ] || ! command -v qemu-arm >/dev/null; then
  QEMU_ARM=${QEMU_ARM:-$PWD/build/host/qemu-arm}
  if [ ! -x "$QEMU_ARM" ]; then
    echo "fetching a static qemu-arm into $QEMU_ARM"
    mkdir -p "$(dirname "$QEMU_ARM")"
    docker run --rm --platform linux/amd64 -v "$(dirname "$QEMU_ARM"):/out" ubuntu:24.04 bash -c \
      "apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq qemu-user-static >/dev/null && cp /usr/bin/qemu-arm-static /out/$(basename "$QEMU_ARM")"
  fi
  exec docker run --rm --platform linux/amd64 -v "$PWD:/src" -v "$QEMU_ARM:/usr/local/bin/qemu-arm:ro" \
    -w /src "$IMAGE" tests/perf/run.sh "$@"
fi

GATE=; SETS="typical heavy"; FRAMES=
while [ $# -gt 0 ]; do
  case $1 in
    --gate) GATE=1 ;;
    --sets) SETS=$2; shift ;;
    --frames) FRAMES=$2; shift ;;
    *) echo "usage: $0 [--gate] [--sets \"typical heavy\"] [--frames N]" >&2; exit 2 ;;
  esac
  shift
done

export PATH=$DEVKITARM/bin:$PATH
OUT=$PWD/build/perf
PERF=tests/perf
GEN=tools/rcheevos-footprint/harness/gen_set.py

# make tracks the .c files; objects are rebuilt from scratch when the build
# rules, the compiler or any header change.
stamp=$( (cat dscheevos/module/Makefile $PERF/perf.mk; arm-none-eabi-gcc --version
  find external/rcheevos dscheevos/core dscheevos/module/include -name '*.h' | sort | xargs cat) | md5sum | cut -c1-32)
if [ "$(cat "$OUT/stamp" 2>/dev/null)" != "$stamp" ]; then rm -rf "$OUT"; fi
mkdir -p "$OUT"; echo "$stamp" > "$OUT/stamp"

for set in $SETS; do
  case $set in
    typical) args="60 8 1 3"; frames=${FRAMES:-300} ;;    # as in docs/rcheevos-arm9-footprint.md
    heavy)   args="250 40 1 6"; frames=${FRAMES:-6} ;;     # overflows max_addresses: the rest is read live
    *) echo "unknown set $set" >&2; exit 2 ;;
  esac
  elf=$OUT/perf-$set.elf
  python3 $GEN $args > "$OUT/set-$set.h.new"
  cmp -s "$OUT/set-$set.h.new" "$OUT/set-$set.h" && rm "$OUT/set-$set.h.new" || mv "$OUT/set-$set.h.new" "$OUT/set-$set.h"
  make -s -C dscheevos/module -f Makefile -f ../../$PERF/perf.mk BUILD="$OUT" PERF_SET=$set "$elf"
  arm-none-eabi-nm -n --special-syms "$elf" | awk '$3 ~ /^\$[atd]/ { print $1, substr($3, 2, 1) }' > "$OUT/map-$set.txt"
  arm-none-eabi-nm "$elf" | awk '$3 ~ /^mark_(vblank|vblank_queued|idle|capture|end)$/ { print $1, $3 }' > "$OUT/marks-$set.txt"
  echo "== $set: $frames frames"
  qemu-arm -cpu arm946 -d in_asm,exec,nochain -D /dev/stdout "$elf" "$frames" 512 |
    awk -v map="$OUT/map-$set.txt" -v marks="$OUT/marks-$set.txt" -f $PERF/count.awk > "$OUT/counts-$set.txt"
  grep -q '^DONE' "$OUT/counts-$set.txt" || { grep -E '^(INFO|FAIL)' "$OUT/counts-$set.txt"; echo "$set: harness did not finish" >&2; exit 1; }
done

python3 $PERF/report.py "$OUT" "$SETS" ${GATE:+--gate $PERF/limits.txt}
