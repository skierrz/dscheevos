#!/bin/bash
# Catalogue game cost: every game of the census (private/census/census.json) through gamecost.sh with one
# module build, so a change is judged on the catalogue's distribution, not on a few games.
# Usage: catalogue.sh TAG MODULE_DIR [OUTROOT]
#   One row per game in OUTROOT/TAG/results.tsv: slug, class (oshalt / bios / busy), set (real / synthetic),
#   cost and equal-work cost (cycles per interval at 67 MHz), shadow I / D, frames snapshotted / evaluated /
#   dropped, lag off / on. Per-game logs in OUTROOT/TAG/<slug>/.
# Window: gameplay games run their census script to its end and measure the last 1500 intervals; menu-only
#   games run the generic script for FRAMES (3000) and measure the last 1500.
# Env: GC_REPO (default the main checkout), everything gamecost.sh needs (dscheevos.env), DSC_PRIVATE, LIB (ROM folder), JOBS (parallel games,
#   default 3; every emulator run goes through emu-lock), ONLY="slug slug" (a subset), FRAMES.
set -u
tag=$1 module=$2 root=${3:-$DSC_PRIVATE/lab/catalogue}
# the checkout whose launcher, loader and tests run the games (built): the main checkout by default
repo=${GC_REPO:-$HOME/Developer/DScheevos/dscheevos}
out=$root/$tag; mkdir -p "$out"
P=${DSC_PRIVATE:?}; L=${LIB:?}
python3 - "$P" "$L" "${ONLY:-}" "${FRAMES:-3000}" > "$out/games.tsv" <<'EOF'
import json, glob, os, sys
P, L, only, frames = sys.argv[1], sys.argv[2], sys.argv[3].split(), int(sys.argv[4])
alias = {'rhythm': 'rhythm-x'}
for g in json.load(open(os.path.join(P, 'census/census.json'))):
    slug = g['slug']
    if only and slug not in only:
        continue
    rom = os.path.join(L, g['rom'])
    if not os.path.exists(rom):
        continue
    sets = glob.glob(os.path.join(P, 'sets', 'sets-%s-*.json' % g.get('gameid', 'x')))
    sc = os.path.join(P, 'census/scripts', alias.get(slug, slug) + '.txt')
    if g.get('script') == 'gameplay' and os.path.exists(sc):
        end = max(int(l.split()[0]) for l in open(sc) if l.strip() and l.split()[0].isdigit())
    else:
        sc, end = os.path.join(P, 'census/scripts/generic.txt'), frames
    print('\t'.join([slug, g['cls'], sets[0] if sets else '-', sc, str(end), rom]))
EOF
one() {
  IFS=$'\t' read -r slug cls set sc end rom <<< "$1"
  d=$out/$slug; mkdir -p "$d"
  [ -s "$d/row.tsv" ] && return 0
  if [ "$set" = - ]; then unset SET; kind=synthetic; else export SET=$set; kind=real; fi
  MODULE=$module SKIP=$((end - 1500)) OUT=$d "$HOME/Developer/DScheevos/bin/emu-lock" -l "catalogue-$slug" "$repo/tests/perf/hardtimeout" 3600 "$repo/tests/perf/gamecost.sh" "$rom" "$end" "$sc" > "$d/gc.txt" 2>&1
  python3 - "$d" "$slug" "$cls" "$kind" > "$d/row.tsv" <<'EOF'
import re, sys, os
d, slug, cls, kind = sys.argv[1:5]
g = open(os.path.join(d, 'gc.txt')).read()
ab = open(os.path.join(d, 'ab.txt')).read() if os.path.exists(os.path.join(d, 'ab.txt')) else ''
def num(pat, t, k=1):
    m = re.search(pat, t, re.M); return m.group(k) if m else '-'
cost = num(r'on - off average: ([+-]?\d+) cycles', g)
eq = num(r'at equal work[^:]*: ([+-]?\d+) cycles', g)
si = num(r'^\s+I-cache\s+(-?\d+)', g); sd = num(r'^\s+D-cache\s+(-?\d+)', g)
st = re.findall(r"'snapshotted': (\d+), 'evaluated': (\d+), 'dropped': (\d+)", ab)
snap, ev, dr = st[-1] if st else ('-', '-', '-')
lag = re.findall(r'achievements (off|on) \(\s*\d+ intervals,\s*(\d+) lag\)', g)
lagd = dict(lag)
print('\t'.join([slug, cls, kind, cost, eq, si, sd, snap, ev, dr, lagd.get('off', '-'), lagd.get('on', '-')]))
EOF
  echo "== $slug $(cut -f4,5,10 "$d/row.tsv")"
}
while IFS= read -r line; do
  while [ "$(jobs -rp | wc -l)" -ge "${JOBS:-3}" ]; do sleep 5; done
  one "$line" &
done < "$out/games.tsv"
wait
{ printf 'slug\tclass\tset\tcost\teq\tshadowI\tshadowD\tsnapshotted\tevaluated\tdropped\tlag_off\tlag_on\n'; cat "$out"/*/row.tsv; } > "$out/results.tsv"
python3 - "$out/results.tsv" <<'EOF'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1]), delimiter='\t'))
F = 1120000 / 100
for cls in ['oshalt', 'bios', 'busy', 'all']:
    r = [x for x in rows if cls == 'all' or x['class'] == cls]
    # shadow I + D: cycles the game lost to our cache use. Raw and equal-work cost also move when a game's own
    # logic diverges between the two runs (Tetris, Spirit Tracks); the shadow doesn't.
    v = sorted(float(x['shadowI']) + float(x['shadowD']) for x in r if x['shadowI'] not in ('-', '') and x['shadowD'] not in ('-', ''))
    if not v: continue
    n = len(v)
    print('%-7s games %3d  shadow I+D: median %.2f%%  p90 %.2f%%  max %.2f%%  over 1%%: %d  over 0.1%%: %d  dropped>0: %d' % (
        cls, n, v[n // 2] / F, v[min(n - 1, n * 9 // 10)] / F, v[-1] / F, sum(x > F for x in v), sum(x > F / 10 for x in v),
        sum(1 for x in r if x['dropped'] not in ('-', '0'))))
EOF
