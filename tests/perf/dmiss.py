#!/usr/bin/env python3
# dmiss.py ELF DMISS.csv INTERVALS: what the module's idle path misses in the D-cache, per region
# (game lines, code/rodata = literal pools and tables, data/bss, heap pieces, arena, stacks; "/c" cached
# view, "/u" the uncached mirror), per data symbol and per heap page, per VBlank interval.
# DMISS.csv: the runner's --profile-idle-dmiss output (kind,addr,count); ELF: the module build's.
# Region bounds follow dscheevos_module.h (DSCM_BASE 0x02D80000 layout).
import csv, sys, subprocess, bisect, collections
elf, path, n = sys.argv[1], sys.argv[2], float(sys.argv[3])
syms = []
marks = {}
for line in subprocess.run(["nm", "-n", "-S", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in "bBdDrR":
        syms.append((int(p[0], 16) & 0xFFFFFF, int(p[1], 16), p[3]))
    if p and p[-1] in ("__text_end", "__heap_start"):
        marks[p[-1]] = int(p[0], 16) & 0xFFFFFF
syms.sort()
starts = [s[0] for s in syms]
heap_start = marks["__heap_start"]
text_end = marks["__text_end"]

def region(a):
    hi, lo = a >> 24, a & 0xFFFFFF
    if hi not in (0x02, 0x0C) or not (0xD80000 <= lo < 0xF80000):
        return "game" if hi == 0x02 else "other-%02x" % hi, None
    view = "c" if hi == 0x02 else "u"
    if lo < text_end: r = "code/rodata"
    elif lo < heap_start: r = "data/bss"
    elif lo < 0xE00000: r = "heap-low"
    elif lo < 0xE00400: r = "session"
    elif lo < 0xE40400: r = "arena"
    elif lo < 0xF7D000: r = "heap-high"
    else: r = "stacks"
    return r + "/" + view, lo

agg = collections.Counter()
sym = collections.Counter()
page = collections.Counter()
lines = collections.Counter()
for r in csv.DictReader(open(path)):
    a = int(r["addr"], 16); c = int(r["count"])
    reg, lo = region(a)
    agg[(r["kind"], reg)] += c
    if r["kind"] == "dmiss_r":
        lines[a & ~31] += c
        if reg.startswith("data/bss") or reg.startswith("code"):
            i = bisect.bisect_right(starts, lo) - 1
            nm = syms[i][2] if i >= 0 and lo < syms[i][0] + max(syms[i][1], 1) else "?"
            sym[nm] += c
        elif reg.startswith("heap") or reg.startswith("arena"):
            page[(reg, lo >> 12)] += c
print("== per region (per interval)")
for (k, reg), c in sorted(agg.items(), key=lambda x: -x[1]):
    print("  %-10s %-16s %10.1f" % (k, reg, c / n))
print("== dmiss_r by data symbol")
for s, c in sym.most_common(15):
    print("  %-40s %8.1f" % (s, c / n))
print("== dmiss_r by heap/arena 4K page")
for (reg, pg), c in page.most_common(25):
    print("  %-12s %06x %8.1f" % (reg, pg << 12, c / n))
print("== distinct read-miss lines in module region:", sum(1 for l in lines if (l >> 24) in (2, 12) and 0xD80000 <= (l & 0xFFFFFF) < 0xF80000))
