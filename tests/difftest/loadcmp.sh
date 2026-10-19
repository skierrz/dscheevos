#!/bin/sh
# tests/difftest/loadcmp.sh SET.json...
# For patches to rcheevos' load path (memref deduplication, parse allocation): loads each set
# with stock and patched rcheevos (loaddump.c) and compares what the load built, byte for byte:
# the memref lists and every condition's operands. Identical structures evaluate identically,
# so this covers sets that have no RAM dump; run.sh still runs on the sets that have one.
# Prints one line per differing set and a summary; exit 1 on any difference.
set -u
here=$(cd "$(dirname "$0")" && pwd)
B=${DTBUILD:-$here/build}
make -s -C "$here" BUILD="$B" ${PATCHED:+PATCHED=$PATCHED} "$B/loaddump-stock" "$B/loaddump-patched" > /dev/null || { echo "FAIL build"; exit 2; }
n=0 bad=0
for set in "$@"; do
  n=$((n + 1))
  a=$("$B/loaddump-stock" "$set" | shasum | cut -c1-16)
  b=$("$B/loaddump-patched" "$set" | shasum | cut -c1-16)
  [ "$a" = "$b" ] || { echo "DIFF $(basename "$set") stock $a patched $b"; bad=$((bad + 1)); }
done
echo "loadcmp: $n sets, $bad differ"
[ $bad = 0 ]
