#!/usr/bin/env python3
# footprint.py ELF FOOTPRINT.csv [top]: the runner's --profile-footprint output summarized: per stretch
# (module idle code between two game instructions) the distinct I lines, own and game D lines and sets,
# light vs pass stretches; per interval totals; the own lines light stretches touch, by symbol
# (a data line is named after the symbol before it: a literal pool shows as its function).
import csv, sys, subprocess, bisect, collections
elf, path = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for line in subprocess.run(["nm", "-n", "-S", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4:
        syms.append((int(p[0], 16), int(p[1], 16), p[3], p[2]))
    elif len(p) == 3 and p[1] in "tTdDbBrR":
        syms.append((int(p[0], 16), 0, p[2], p[1]))
syms.sort()
starts = [s[0] for s in syms]
def name(a):
    lo = a & 0xFFFFFF | 0x02000000
    if (a >> 24) == 0x02 and 0xF7E000 <= (a & 0xFFFFFF) < 0xF80000:
        return "stack top-0x%x" % (0x2F80000 - (a & ~31 & 0xFFFFFFF))
    if not (0xD80000 <= (a & 0xFFFFFF) < 0xF80000) or (a >> 24) not in (2, 0xC):
        return "game"
    best = None
    for b in (a, (a & 0xFFFFFF) | 0x0C000000, (a & 0xFFFFFF) | 0x02000000):
        i = bisect.bisect_right(starts, b + 31) - 1
        # symbols overlapping the line [b, b+32)
        while i >= 0 and syms[i][0] + max(syms[i][1], 1) <= b and syms[i][1]:
            i -= 1
        if i >= 0 and syms[i][0] >> 24 == b >> 24:
            s = syms[i]
            if s[0] <= b + 31:
                best = s[2] + ("" if s[0] + s[1] > b or not s[1] else "+pool?")
                break
    return best or "?"
summ, lines = {}, []
for row in csv.reader(open(path)):
    if row[0] == "summary" and row[1] != "class":
        summ[row[1]] = [int(x) for x in row[2:]]
    elif row[0] == "line" and row[1] != "kind":
        lines.append((row[1], int(row[2], 16), int(row[3]), int(row[4])))
iv = max(v[0] for v in summ.values()) or 1
print("class   stretches/iv  I lines  I sets  Downlines Downsets  Dgame lines  Dgame sets  D sets (per stretch)")
for c, v in summ.items():
    n = v[1] or 1
    print("%-6s %10.1f %9.1f %7.1f %9.1f %8.1f %11.1f %10.1f %7.1f" % (c, v[1] / iv, v[2]/n, v[3]/n, v[4]/n, v[5]/n, v[6]/n, v[7]/n, v[8]/n))
tot = [sum(v[i] for v in summ.values()) / iv for i in (1, 2, 4, 6)]
print("per interval (all stretches): stretches %.1f  I line-touches %.0f  D own line-touches %.0f  D game line-touches %.0f" % tuple(tot))
L = summ.get("light", [0, 1])[1] or 1
print("\nlines touched by light stretches (fraction of light stretches), own lines:")
own = [(k, a, l, p) for k, a, l, p in lines if name(a) != "game"]
for k, a, l, p in sorted(own, key=lambda x: -x[2])[:top]:
    if l == 0: break
    print("  %s %08x set %2d  %.2f  %s" % (k, a, (a >> 5) & (63 if k == "I" else 31), l / L, name(a)))
agg = collections.defaultdict(float)
for k, a, l, p in own:
    agg[(k, name(a).split("+")[0])] += l / L
print("\nper light stretch, by symbol:")
for (k, s), v in sorted(agg.items(), key=lambda x: -x[1])[:top]:
    if v < 0.05: break
    print("  %s %6.2f %s" % (k, v, s))
