#!/usr/bin/env python3
"""bgdiff.py META DUMP_A DUMP_B: address-level difference of two --bgate-dump files (same record, two runs).
Prints differing address ranges per region and byte counts only, never contents."""
import sys
meta, fa, fb = sys.argv[1:4]
regs = []
for line in open(meta):
    w = line.split()
    if w and w[0] == 'region':
        regs.append((w[1], int(w[2], 16), int(w[3], 16)))
a = open(fa, 'rb').read(); b = open(fb, 'rb').read()
if len(a) != len(b):
    raise SystemExit('dump sizes differ')
off = 0
limit = int(sys.argv[4]) if len(sys.argv) > 4 else 40
for name, base, size in regs:
    x, y = a[off:off + size], b[off:off + size]; off += size
    if x == y:
        continue
    ranges = []; i = 0
    while i < size:
        if x[i] != y[i]:
            j = i
            while j < size and (x[j] != y[j] or (j + 1 < size and x[j + 1] != y[j + 1]) or (j + 2 < size and x[j + 2] != y[j + 2]) or (j + 3 < size and x[j + 3] != y[j + 3])):
                j += 1
            ranges.append((i, j)); i = j
        else:
            i += 1
    n = sum(1 for i in range(size) if x[i] != y[i])
    print('%s: %d bytes differ in %d ranges' % (name, n, len(ranges)))
    for lo, hi in ranges[:limit]:
        print('  %08x-%08x (%d bytes)' % (base + lo, base + hi, hi - lo))
    if len(ranges) > limit:
        print('  ...')
