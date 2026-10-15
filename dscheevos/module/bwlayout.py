#!/usr/bin/env python3
# LAB coverage: bwlayout.py DEFAULT.nm BW.nm : the BW build's linker script (module-bw.ld, hot-bw.ld) that keeps the
# default build's hot layout. Each hot function the BW build grows (yield-point calls) keeps a placeholder of its
# default size in its hot.ld slot, so every other hot function stays at its default address; the grown functions go
# after the warm code, each at its default address's cache colour (mod 0x800). Inputs: `nm -n -S` of a
# default build and of a BW build linked with the plain module.ld. Regenerate when hot code changes.
import re, sys
def syms(p):
    d = {}
    for l in open(p):
        x = l.split()
        if len(x) == 4: d.setdefault(x[3], (int(x[0], 16), int(x[1], 16)))
    return d
D, B = syms(sys.argv[1]), syms(sys.argv[2])
out, moved = [], []
for line in open('hot.ld'):
    m = re.search(r'\(\.text\.([^)]+)\)', line)
    if m and m.group(1) in D and m.group(1) in B and B[m.group(1)][1] != D[m.group(1)][1]:
        name = m.group(1); out.append(f'    . += 0x{D[name][1]:x};   /* {name}: its default size (grown in the BW build) */\n')
        moved.append((line.strip(), name)); continue
    out.append(line)
open('hot-bw.ld', 'w').write(''.join(out))
grown = []
for line, name in moved:
    if True:   # each at its default address's I-cache colour (mod 0x800): same sets as in the default build
        col = D[name][0] & 0x7FF
        grown.append(f'    . = ALIGN(0x800) + 0x{col:x} - (((ABSOLUTE(.) & 0x7FF) != 0 && (ABSOLUTE(.) & 0x7FF) <= 0x{col:x}) ? 0x800 : 0);\n')
    grown.append(f'    {line}\n')
s = open('module.ld').read()
s = s.replace('INCLUDE hot.ld', 'INCLUDE hot-bw.ld')
s = s.replace('    INCLUDE warm.ld\n    __warm_end = .;\n', '    INCLUDE warm.ld\n    __warm_end = .;\n    /* LAB coverage: the hot functions the BW build grows (bwlayout.py) */\n' + ''.join(grown))
open('module-bw.ld', 'w').write(s)
print('grown:', ', '.join(n for _, n in moved))
