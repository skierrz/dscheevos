#!/bin/sh
# Remove stale soft-locked work dirs: only holders under the work root whose owner pid (README "pid:" line)
# is no longer running, plus their run dir and the runner's sibling .img files. A live run is never touched.
# Run dirs without a holder are listed, not removed. Usage: tests/lib/workdir-gc.sh [-n]   (-n: dry run)
here=$(cd "$(dirname "$0")" && pwd)
. "$here/workdir.sh"
dry=0; [ "${1:-}" = -n ] && dry=1
r=$(dsc_work_root) || exit 1
for h in "$r"/*/; do
  h=${h%/}; [ -f "$h/README" ] || continue
  pid=$(sed -n 's/^pid: *//p' "$h/README" | head -1)
  case $pid in ''|*[!0-9]*) echo "skip $h (no pid)"; continue ;; esac
  if kill -0 "$pid" 2>/dev/null; then echo "live  $h (pid $pid)"; continue; fi
  echo "stale $h (pid $pid gone)"
  [ $dry = 1 ] || dsc_rmwork "$h"
done
for d in "$r.d"/*/; do
  d=${d%/}; [ -d "$d" ] || continue
  [ -e "$r/${d##*/}" ] || echo "no holder: $d (not removed)"
done
exit 0
