#!/usr/bin/env python3
"""Print the DScheevos module status from a main RAM dump (melonds-headless --dump-ram).

  status.py ram.bin dscheevos9.map [--expect ID ...] [--max-dropped N]
"""
import argparse
import re
import struct
import sys

ap = argparse.ArgumentParser()
ap.add_argument('ram')
ap.add_argument('map')
ap.add_argument('--expect', type=int, action='append', default=[], help='achievement ID that must have unlocked')
ap.add_argument('--never', type=int, action='append', default=[], help='achievement ID that must not unlock')
ap.add_argument('--max-dropped', type=int, default=0)
args = ap.parse_args()
ram = open(args.ram, 'rb').read()
mp = open(args.map).read()
addr = int(re.search(r'0x([0-9a-f]+)\s+dscm_g_status\b', mp).group(1), 16)
o = addr - 0x02000000
f = struct.unpack_from('<8I', ram, o)
names = 'active hardcore snapshotted evaluated dropped missed eval_lines unlock_count'.split()
print(dict(zip(names, f)))
unlocked = set()
for i in range(min(f[7], 8)):
    id_, pts, fr = struct.unpack_from('<3I', ram, o + 32 + i * 60)
    title = ram[o + 44 + i * 60:o + 92 + i * 60].split(b'\0')[0].decode()
    print(f'  unlock {id_} ({pts} pts) at frame {fr}: {title}')
    unlocked.add(id_)
p = o + 32 + 8 * 60
pending, err = struct.unpack_from('<2I', ram, p)
print('pending', pending, 'error', err, 'title', ram[p + 8:p + 56].split(b'\0')[0].decode())
hdr = struct.unpack_from('<10I', ram, 0xD80000)
print('header magic %08x version %d vblank_count %d' % (hdr[0], hdr[1], hdr[9]))

fails = []
if hdr[0] != 0x56435344:
    fails.append('module not loaded')
if not f[0]:
    fails.append('not active')
if err:
    fails.append(f'error {err}')
if f[4] > args.max_dropped:
    fails.append(f'{f[4]} frames dropped')
fails += [f'{i} did not unlock' for i in args.expect if i not in unlocked]
fails += [f'{i} unlocked' for i in args.never if i in unlocked]
print('RESULT ' + ('FAIL: ' + '; '.join(fails) if fails else 'PASS'))
sys.exit(1 if fails else 0)
