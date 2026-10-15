#!/usr/bin/env python3
"""wdiff.py A.csv B.csv [N]: compare two --watch-log files (the same watched range, reference vs run).
Walks both write sequences in order and prints the first N places where the writer (cpu, pc, addr, size) or the
written value differs. Values are compared, never printed."""
import csv, sys
a = list(csv.DictReader(open(sys.argv[1]))); b = list(csv.DictReader(open(sys.argv[2])))
n = int(sys.argv[3]) if len(sys.argv) > 3 else 10
key = lambda r: (r['cpu'], r['pc'], r['addr'], r['bits'])
print('writes: %d vs %d' % (len(a), len(b)))
shown = 0
for i in range(min(len(a), len(b))):
    x, y = a[i], b[i]
    if key(x) != key(y) or x['value'] != y['value']:
        what = 'value differs' if key(x) == key(y) else 'writer differs: ARM%s pc %s lr %s -> %s vs ARM%s pc %s lr %s -> %s' % (
            y['cpu'], y['pc'], y['lr'], y['addr'], x['cpu'], x['pc'], x['lr'], x['addr'])
        print('#%d frame %s/%s ts %s/%s: ARM%s pc %s lr %s mode %s writes %s (%s bits): %s' % (
            i, x['frame'], y['frame'], x['ts'], y['ts'], x['cpu'], x['pc'], x['lr'], x['mode'], x['addr'], x['bits'], what))
        shown += 1
        if shown >= n:
            break
if not shown:
    print('identical over the common length')
