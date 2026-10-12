#!/bin/bash
# Per-frame ARM946 instruction counts for real RetroAchievements sets, through
# the DScheevos core (dsc_capture, dsc_idle) and rc_client_do_frame alone, built
# with the module's own rules and flags (tests/perf/realsets.c).
#
#   tests/perf/realsets.sh <dir with sets-<game>-<hash>.json> [game id...]
#
# Optional: FRAMES (default 60), BYTES (bytes of RAM changed per frame, 512),
#   FLOAT=1 (also the float-operand variant, information only),
#   RAM_<game id>=<file> (a game RAM dump to use instead of random RAM).
# Output: build/realsets/counts.tsv and the table from realsets_report.py.
# Sets are the server's achievementsets replies; they are not in the repo.
#
# Needs Docker: devkitpro/devkitarm:20241104 to build, and the image from
# tests/perf/qplug (qemu-arm with a counting plugin, built on first use) to run.
set -eo pipefail
cd "$(dirname "$0")/../.."
[ $# -ge 1 ] || { sed -n '2,15p' "$0"; exit 2; }
SETS=$(cd "$1" && pwd); shift
IDS="$*"
[ -n "$IDS" ] || IDS=$(ls "$SETS" | sed -n 's/^sets-\([0-9]*\)-.*\.json$/\1/p' | sort -n | tr '\n' ' ')
FRAMES=${FRAMES:-60} BYTES=${BYTES:-512}
QIMAGE=dscheevos-qplug:8.2.2
OUT=${OUT:-build/realsets}
VARIANTS="stock"; [ "${FLOAT:-0}" = 1 ] && VARIANTS="stock float"

docker image inspect $QIMAGE >/dev/null 2>&1 || docker build -t $QIMAGE tests/perf/qplug

docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 bash -c '
  set -e; export PATH=$DEVKITARM/bin:$PATH
  for v in '"$VARIANTS"'; do
    x=; [ $v = float ] && x="-include /src/tests/perf/realsets-float.h"
    make -s -C dscheevos/module -f Makefile -f ../../tests/perf/realsets.mk BUILD=/src/'$OUT'/$v EXTRA_CFLAGS="$x" /src/'$OUT'/$v/realsets.elf
    arm-none-eabi-nm /src/'$OUT'/$v/realsets.elf | awk '"'"'$3=="mark_begin"{b=$1} $3=="mark_end"{e=$1} END{print b, e}'"'"' > /src/'$OUT'/$v/marks.txt
  done'

ramdirs=; for id in $IDS; do eval "r=\${RAM_$id:-}"; [ -z "$r" ] || ramdirs="$ramdirs -v $(cd "$(dirname "$r")" && pwd)/$(basename "$r"):/ram/$id:ro"; done
: > $OUT/counts.tsv
for v in $VARIANTS; do
  read -r mb me < $OUT/$v/marks.txt
  for id in $IDS; do
    set_file=$(cd "$SETS" && ls sets-$id-*.json | head -1)
    eval "r=\${RAM_$id:-}"; ram=; [ -z "$r" ] || ram=/ram/$id
    for mode in core rc; do
      line=$(docker run --rm -v "$PWD/$OUT:/b:ro" -v "$SETS:/sets:ro" $ramdirs $QIMAGE \
        qemu-arm-plug -cpu arm946 -plugin /usr/local/lib/icount.so,mb=$mb,me=$me \
        /b/$v/realsets.elf $mode /sets/$set_file $FRAMES $BYTES $ram 2>&1 | tr '\n' ' ')
      case $line in *DONE*) ;; *) echo "$id $v $mode: $line" >&2 ;; esac
      printf '%s\t%s\t%s\t%s\t%s\n' "$id" "$v" "$mode" "${ram:+dump}" "$line" >> $OUT/counts.tsv
      echo "$id $v $mode done"
    done
  done
done
python3 tests/perf/realsets_report.py $OUT/counts.tsv "$SETS"
