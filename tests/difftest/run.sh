#!/bin/sh
# tests/difftest/run.sh WORK SET.json BASE.bin [FRAMES] [SPLITS]
# Generates the inputs into WORK (private: sets and RAM stay out of the repo),
# runs stock and patched over every script x session x hardcore, and compares:
# patched with no features (inert), with the partition feature and no split,
# and with each split (threaded too); change-driven evaluation alone and with
# the partition (ONLY_CD=1: just those two). Prints one line per comparison; exit 1
# on any difference. Heavy: run through build-lock.
set -u
here=$(cd "$(dirname "$0")" && pwd)
work=$1 set=$2 base=$3 frames=${4:-10000} nsplits=${5:-20}
B=$here/build
mkdir -p "$work/scripts" "$work/logs"
python3 "$here/gen.py" session "$set" none 1 > "$work/session-none.json"
python3 "$here/gen.py" session "$set" half 2 > "$work/session-half.json"
python3 "$here/gen.py" fuzz "$set" "$base" 12345 "$frames" > "$work/fuzz.dtrc"
python3 "$here/gen.py" scripts "$set" 7 "$frames" "$work/scripts"
python3 "$here/gen.py" splits "$set" 99 "$nsplits" > "$work/splits.txt"
fail=0 runs=0
for script in "$work"/scripts/*.txt; do
 for sess in none half; do
  for hc in 1 0; do
   tag=$(basename "$script" .txt)-$sess-hc$hc
   common="--set $set --session $work/session-$sess.json --trace $work/fuzz.dtrc --script $script --hardcore $hc"
   "$B/difftest-run-stock" $common --out "$work/logs/$tag-stock.log" || { echo "FAIL run stock $tag"; fail=1; continue; }
   variants="inert| partition-none|--mode=partition,--features=partition,--split=none"
   while read -r sp; do variants="$variants split-$(echo $sp | tr ':' '-')|--mode=partition,--features=partition,--split=$sp"; done < "$work/splits.txt"
   variants="$variants thread-all|--mode=partition,--features=partition,--split=all,--thread"
   variants="$variants cd|--features=changedriven cd-split-all|--mode=partition,--features=partition+changedriven,--split=all"
   [ -n "${ONLY_CD:-}" ] && variants="cd|--features=changedriven cd-split-all|--mode=partition,--features=partition+changedriven,--split=all"
   for v in $variants; do
     name=${v%%|*}; args=$(echo "${v#*|}" | tr ',' ' ' | sed "s/=/ /g")
     log="$work/logs/$tag-$name.log"
     "$B/difftest-run-patched" $common $args --out "$log" || { echo "FAIL run $tag $name"; fail=1; continue; }
     runs=$((runs + 1))
     if ! python3 "$here/cmp.py" "$work/logs/$tag-stock.log" "$log" > "$work/logs/$tag-$name.cmp"; then
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
