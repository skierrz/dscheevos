#!/bin/sh
# tests/difftest/run.sh WORK SET.json BASE.bin [FRAMES] [SPLITS]
# Generates the inputs into WORK (private: sets and RAM stay out of the repo),
# runs stock and patched over every script x session x hardcore, and compares:
# the patched tree as built ("inert": its built-in changes, no opt-in feature on);
# PARTITION=1 adds the partition feature (no split, each split, threaded); CD=1 adds
# change-driven evaluation on; CHANGES=1 adds rc_client_set_changed_memory with the trace's exact
# changes (and, with CD=1, both), and the fuzzed trace's indirect-read stressors. Prints one line per failing comparison; exit 1
# on any difference. TRACE=file replays a recorded trace instead of the fuzzed one.
# Heavy: run through build-lock.
set -u
here=$(cd "$(dirname "$0")" && pwd)
work=$1 set=$2 base=$3 frames=${4:-10000} nsplits=${5:-20}
B=${DTBUILD:-$here/build}
# never run stale binaries: rebuild against the current trees first (PATCHED=dir overrides the tree,
# DTBUILD=dir the build directory, so runs against different trees don't share binaries)
make -s -C "$here" BUILD="$B" ${PATCHED:+PATCHED=$PATCHED} > /dev/null || { echo "FAIL build"; exit 2; }
mkdir -p "$work/scripts" "$work/logs"
python3 "$here/gen.py" session "$set" none 1 > "$work/session-none.json"
python3 "$here/gen.py" session "$set" half 2 > "$work/session-half.json"
if [ -n "${TRACE:-}" ]; then
  # a recorded trace (e.g. real play) instead of the fuzzed one; FRAMES = its frame count
  cp "$TRACE" "$work/fuzz.dtrc"
  frames=$(python3 -c "import struct,sys;f=open(sys.argv[1],'rb');f.read(8);print(struct.unpack('<IIII',f.read(16))[2])" "$TRACE")
else
  python3 "$here/gen.py" fuzz "$set" "$base" 12345 "$frames" ${CHANGES:+stress} > "$work/fuzz.dtrc"
fi
python3 "$here/gen.py" scripts "$set" 7 "$frames" "$work/scripts"
python3 "$here/gen.py" splits "$set" 99 "$nsplits" > "$work/splits.txt"
fail=0 runs=0
for script in "$work"/scripts/*.txt; do
 for sess in none half; do
  for hc in 1 0; do
   tag=$(basename "$script" .txt)-$sess-hc$hc
   common="--set $set --session $work/session-$sess.json --trace $work/fuzz.dtrc --script $script --hardcore $hc"
   "$B/difftest-run-stock" $common --out "$work/logs/$tag-stock.log" || { echo "FAIL run stock $tag"; fail=1; continue; }
   # the patched tree's own features: always "inert" (built in, no switch); the partition and
   # change-driven variants only when PARTITION=1 / CD=1 (their patches are in that tree)
   variants="inert|"
   if [ -n "${PARTITION:-}" ]; then
     variants="$variants partition-none|--mode=partition,--features=partition,--split=none"
     while read -r sp; do variants="$variants split-$(echo $sp | tr ':' '-')|--mode=partition,--features=partition,--split=$sp"; done < "$work/splits.txt"
     variants="$variants thread-all|--mode=partition,--features=partition,--split=all,--thread"
   fi
   [ -n "${CD:-}" ] && variants="$variants cd|--features=changedriven cdsparse|--features=changedriven,--state-sparse=37"
   [ -n "${CHANGES:-}" ] && variants="$variants changes|--features=changes"
   [ -n "${CHANGES:-}" ] && [ -n "${CD:-}" ] && variants="$variants cd+changes|--features=changedriven+changes"
   for v in $variants; do
     name=${v%%|*}; args=$(echo "${v#*|}" | tr ',' ' ' | sed "s/=/ /g")
     log="$work/logs/$tag-$name.log"
     ref="$work/logs/$tag-stock.log"
     if [ "$name" = cdsparse ]; then
       # the stock reference with the same sparse state blocks
       ref="$work/logs/$tag-stock-sparse.log"
       [ -f "$ref" ] || "$B/difftest-run-stock" $common --state-sparse 37 --out "$ref" || { echo "FAIL run stock sparse $tag"; fail=1; continue; }
     fi
     "$B/difftest-run-patched" $common $args --out "$log" || { echo "FAIL run $tag $name"; fail=1; continue; }
     runs=$((runs + 1))
     if ! python3 "$here/cmp.py" "$ref" "$log" > "$work/logs/$tag-$name.cmp"; then
       echo "DIFF $tag $name: $(head -1 "$work/logs/$tag-$name.cmp")"; fail=1
     else
       rm -f "$log" "$work/logs/$tag-$name.cmp"
     fi
   done
  done
 done
done
echo "runs $runs, $( [ $fail = 0 ] && echo 'zero differences' || echo 'DIFFERENCES' )"
exit $fail
