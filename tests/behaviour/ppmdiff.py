#!/usr/bin/env python3
"""ppmdiff.py A.ppm B.ppm: which scanlines of the two screens (256x384 PPM from the runner) differ, and how many pixels."""
import sys
def load(p):
    d = open(p, 'rb').read(); parts = d.split(b'\n', 3); return parts[3]
a, b = load(sys.argv[1]), load(sys.argv[2])
rows = []
for y in range(384):
    x0, x1 = a[y * 768:(y + 1) * 768], b[y * 768:(y + 1) * 768]
    if x0 != x1:
        n = sum(1 for i in range(256) if x0[i * 3:i * 3 + 3] != x1[i * 3:i * 3 + 3])
        xs = [i for i in range(256) if x0[i * 3:i * 3 + 3] != x1[i * 3:i * 3 + 3]]
        rows.append((y, n, xs[0], xs[-1]))
print('%d rows differ' % len(rows))
for y, n, lo, hi in rows[:60]:
    print('  %s line %3d: %3d pixels (x %d-%d)' % ('top' if y < 192 else 'bottom', y % 192, n, lo, hi))
