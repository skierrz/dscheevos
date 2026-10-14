#!/bin/sh
# tests/difftest/core.sh WORK SET.json BASE.bin [FRAMES]
# Hand-over gate G0 (research/offload/handover-step0.md): the DScheevos core,
# fed the trace one capture and VBlank per frame, against stock rcheevos run
# directly, over the lifecycle scripts the core supports (clean, resets,
# hardcore off, pauses, award failures, hit counts near 2^32) x both session
# replies x hardcore on/off. Variants: the core with the module's flags and
# two evaluators handing the rc_client over every random 1-8 frames (ho), the
# same without the flags (ho-noflags), and the flags without hand-over (plain).
# TRACE=file replays a recorded trace. Prints one line per failing comparison
# and a summary; exit 1 on any difference. Heavy: run through build-lock.
set -u
here=$(cd "$(dirname "$0")" && pwd)
work=$1 set=$2 base=$3 frames=${4:-2000}
B=${DTBUILD:-$here/build}
make -s -C "$here" BUILD="$B" "$B/difftest-run-stock" core > /dev/null || { echo "FAIL build"; exit 2; }
mkdir -p "$work/scripts" "$work/logs"
python3 "$here/gen.py" session "$set" none 1 > "$work/session-none.json"
python3 "$here/gen.py" session "$set" half 2 > "$work/session-half.json"
if [ -n "${TRACE:-}" ]; then
  cp "$TRACE" "$work/fuzz.dtrc"
  frames=$(python3 -c "import struct,sys;f=open(sys.argv[1],'rb');f.read(8);print(struct.unpack('<IIII',f.read(16))[2])" "$TRACE")
else
  python3 "$here/gen.py" fuzz "$set" "$base" 12345 "$frames" > "$work/fuzz.dtrc"
fi
python3 "$here/gen.py" scripts "$set" 7 "$frames" "$work/scripts"
rm -f "$work/scripts/5-shortread.txt" "$work/scripts/7-reload.txt"   # not core features (see run.c, core mode)
fail=0 runs=0 seed=1
for script in "$work"/scripts/*.txt; do
 for sess in none half; do
  for hc in 1 0; do
   tag=$(basename "$script" .txt)-$sess-hc$hc
   common="--set $set --session $work/session-$sess.json --trace $work/fuzz.dtrc --script $script --hardcore $hc"
   "$B/difftest-run-stock" $common --out "$work/logs/$tag-stock.log" || { echo "FAIL run stock $tag"; fail=1; continue; }
   seed=$((seed * 1103515245 % 2147483647 + 12345))
   vs="ho|difftest-run-core|--handover $seed;ho-noflags|difftest-run-core-noflags|--handover $((seed ^ 0x5a5a));plain|difftest-run-core|"
   # YIELD=1 (LAB coverage): also the rcheevos yield points with captures fed mid-evaluation; YIELD=only: just that
   [ "${YIELD:-}" = 1 ] && vs="$vs;yield|difftest-run-core-yield|"
   [ "${YIELD:-}" = only ] && vs="yield|difftest-run-core-yield|"
   old_ifs=$IFS; IFS=';'; set -- $vs; IFS=$old_ifs
   for v in "$@"; do
     name=${v%%|*}; rest=${v#*|}; bin=${rest%%|*}; args=${rest#*|}
     log="$work/logs/$tag-$name.log"
     "$B/$bin" $common $args --out "$log" || { echo "FAIL run $tag $name"; fail=1; continue; }
     runs=$((runs + 1))
     if grep -q "dropped [1-9]" "$log"; then echo "DROPPED $tag $name: $(grep '^# core' "$log")"; fail=1; fi
     if ! python3 "$here/cmp.py" "$work/logs/$tag-stock.log" "$log" > "$work/logs/$tag-$name.cmp"; then
       echo "DIFF $tag $name: $(head -1 "$work/logs/$tag-$name.cmp")"; fail=1
     else
       grep '^# core\|^# yield' "$log" | sed "s/^# core:/STAT $tag $name:/; s/^# yield:/YIELD $tag $name:/" >> "$work/stats.txt"
       rm -f "$log" "$work/logs/$tag-$name.cmp"
     fi
   done
  done
 done
done
echo "runs $runs, $( [ $fail = 0 ] && echo 'zero differences' || echo 'DIFFERENCES' )"
exit $fail
