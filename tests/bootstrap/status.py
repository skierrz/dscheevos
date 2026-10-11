#!/usr/bin/env python3
"""Print the DScheevos module status from a main RAM dump (melonds-headless --dump-ram).

  status.py ram.bin dscheevos9.map [--expect ID ...] [--max-dropped N]
"""
import argparse
import re
import struct
import sys

def hdr_version(ram):
    return struct.unpack_from('<I', ram, 0xD80004)[0]


ap = argparse.ArgumentParser()
ap.add_argument('ram')
ap.add_argument('map')
ap.add_argument('--expect', type=int, action='append', default=[], help='achievement ID that must have unlocked')
ap.add_argument('--never', type=int, action='append', default=[], help='achievement ID that must not unlock')
ap.add_argument('--max-dropped', type=int, default=0)
ap.add_argument('--expect-sent', type=int, help='unlocks the server must have answered during play')
ap.add_argument('--expect-rejoins', type=int, help='times the module must have rejoined the access point')
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
if len(ram) >= p + 84 and hdr_version(ram) >= 4:
    ach, ach_un, pts, pts_un, nstate, ndetail, sent, hs_ms = struct.unpack_from('<8I', ram, p + 56)
    states = ['none', 'connecting', 'online', 'sending', 'paused', 'offline', 'error']
    print(f'progress {ach_un}/{ach} achievements, {pts_un}/{pts} pts; network {states[nstate] if nstate < len(states) else nstate}'
          f' (detail {ndetail:#x}), unlocks sent {sent}, last TLS handshake {hs_ms} ms')
rejoins = struct.unpack_from('<I', ram, p + 88)[0] if hdr_version(ram) >= 5 else 0
if hdr_version(ram) >= 5:
    print(f'rejoined the access point {rejoins} times')
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
if args.expect_sent is not None:
    sent_now = struct.unpack_from('<I', ram, p + 80)[0] if hdr_version(ram) >= 4 else 0
    if sent_now < args.expect_sent:
        fails.append(f'{sent_now} unlocks sent, expected {args.expect_sent}')
if args.expect_rejoins is not None and rejoins < args.expect_rejoins:
    fails.append(f'rejoined {rejoins} times, expected {args.expect_rejoins}')
fails += [f'{i} unlocked' for i in args.never if i in unlocked]
print('RESULT ' + ('FAIL: ' + '; '.join(fails) if fails else 'PASS'))
sys.exit(1 if fails else 0)
