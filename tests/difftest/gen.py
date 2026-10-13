#!/usr/bin/env python3
"""difftest inputs (docs/rcheevos-difftest.md).

  gen.py session SET.json none|half SEED > session.json
      a startsession reply: no unlocks, or about half the achievements unlocked
      (some hardcore, some casual only).
  gen.py fuzz SET.json BASE.bin SEED FRAMES > trace.dtrc
      a fuzzed trace from a base image (4 MB main RAM + 16 KB DTCM, e.g. a RAM
      dump): mutations on the set's watched addresses (random bytes and words,
      sweeps through each condition's constants, freezes of 1-600 frames,
      toggles, pointer moves, short reads at the end of main RAM).
  gen.py scripts SET.json SEED FRAMES OUTDIR
      the seven lifecycle scripts (clean, resets, hardcore off, pauses, short
      read, award failures, unload/reload).
  gen.py splits SET.json SEED N
      N random --split specs (and the extremes) for partition runs.

Trace format (difftest-run reads it): b"DTRC1\\0\\0\\0", u32 main size, u32 dtcm
size, u32 frames, u32 seed, main image, dtcm image, then per frame: u32 frame,
u8 flags (1 LAG, 2 MENU), u8 0, u16 ranges, per range: u32 address
(rcheevos' DS address space: main RAM at 0, DTCM at 0x1000000), u16 length, bytes.
"""
import json
import random
import re
import struct
import sys

MAIN = 0x400000
DTCM = 0x4000
DTCM_BASE = 0x1000000

SIZES = {'H': 1, 'L': 1, 'U': 1, 'K': 1, 'M': 1, 'N': 1, 'O': 1, 'P': 1, 'Q': 1, 'R': 1, 'S': 1, 'T': 1,
         '': 2, ' ': 2, 'I': 2, 'W': 3, 'J': 3, 'X': 4, 'G': 4}
OPERAND = re.compile(r'(?<![0-9A-Za-z])(?:[dpb~]?)(0x([A-Z ]?)|f([A-Za-z]))([0-9a-fA-F]+)')
COMPARE = re.compile(r'(?<![0-9A-Za-z])(?:[dpb~]?)(?:0x([A-Z ]?)|f[A-Za-z])([0-9a-fA-F]+)\s*(?:=|!=|<=|>=|<|>)\s*(h[0-9a-fA-F]+|-?\d+)')


def load_set(path):
    with open(path) as f:
        return json.load(f)


def memaddrs(s):
    out = []
    for st in s.get('Sets', []):
        for a in st.get('Achievements', []):
            out.append(a.get('MemAddr', ''))
        for lb in st.get('Leaderboards', []) or []:
            out.append(lb.get('Mem', ''))
    out.append(s.get('RichPresencePatch', '') or '')
    return out


def watched(s):
    """(address, size), constants per address, AddAddress chains (base, size, [offsets])"""
    addrs, consts, chains = {}, {}, []
    for m in memaddrs(s):
        for g in OPERAND.finditer(m):
            size = SIZES.get(g.group(2) or '', 4) if g.group(2) is not None else 4
            a = int(g.group(4), 16)
            addrs[a] = max(addrs.get(a, 0), size)
        for g in COMPARE.finditer(m):
            size = SIZES.get(g.group(1) or '', 4) if g.group(1) is not None else 4
            a = int(g.group(2), 16)
            v = g.group(3)
            v = int(v[1:], 16) if v.startswith('h') else int(v)
            consts.setdefault(a, set()).add((size, v))
        # AddAddress: "I:<operand>" then the next operands are offsets from it
        for cond_group in re.split(r'[SR]?(?:\$|::)', m):
            conds = cond_group.split('_')
            for i, c in enumerate(conds):
                if c.startswith('I:'):
                    g = OPERAND.search(c)
                    if not g:
                        continue
                    base = int(g.group(4), 16)
                    size = SIZES.get(g.group(2) or '', 4) if g.group(2) is not None else 4
                    offs = []
                    for nxt in conds[i + 1:i + 3]:
                        h = OPERAND.search(nxt)
                        if h:
                            offs.append(int(h.group(4), 16))
                    chains.append((base, size, offs))
    return addrs, consts, chains


def in_region(a, n):
    return (a + n <= MAIN) or (DTCM_BASE <= a and a + n <= DTCM_BASE + DTCM)


def cmd_session(setpath, kind, seed):
    s = load_set(setpath)
    rnd = random.Random(int(seed))
    ids = [a['ID'] for st in s.get('Sets', []) for a in st.get('Achievements', [])]
    hard, soft = [], []
    if kind == 'half':
        for i in ids:
            r = rnd.random()
            if r < 0.3:
                hard.append(i)
            elif r < 0.5:
                soft.append(i)
    elif kind != 'none':
        sys.exit('session kind: none|half')
    when = 1690000000
    reply = {'Success': True,
             'HardcoreUnlocks': [{'ID': i, 'When': when} for i in hard],
             'Unlocks': [{'ID': i, 'When': when} for i in hard + soft],
             'ServerNow': when + 1000}
    sys.stdout.write(json.dumps(reply))


class Trace:
    def __init__(self, main, dtcm):
        self.main = bytearray(main)
        self.dtcm = bytearray(dtcm)
        self.frames = []

    def read(self, a, n):
        if a + n <= MAIN:
            return int.from_bytes(self.main[a:a + n], 'little')
        if DTCM_BASE <= a and a + n <= DTCM_BASE + DTCM:
            o = a - DTCM_BASE
            return int.from_bytes(self.dtcm[o:o + n], 'little')
        return None

    def write(self, ranges, a, n, v):
        if not in_region(a, n):
            return
        b = (v & ((1 << (8 * n)) - 1)).to_bytes(n, 'little')
        if a < MAIN:
            self.main[a:a + n] = b
        else:
            self.dtcm[a - DTCM_BASE:a - DTCM_BASE + n] = b
        ranges.append((a, b))


def cmd_fuzz(setpath, basepath, seed, frames):
    s = load_set(setpath)
    seed, frames = int(seed), int(frames)
    rnd = random.Random(seed)
    with open(basepath, 'rb') as f:
        img = f.read()
    if len(img) < MAIN:
        sys.exit('base image: at least 4 MB')
    main = img[:MAIN]
    dtcm = img[MAIN:MAIN + DTCM] if len(img) >= MAIN + DTCM else bytes(DTCM)
    t = Trace(main, dtcm)
    addrs, consts, chains = watched(s)
    alist = sorted(a for a in addrs if in_region(a, 1))
    clist = sorted((a, n, v) for a, vs in consts.items() for (n, v) in vs if in_region(a, n))
    if not alist:
        sys.exit('no watched addresses')
    out = bytearray(b'DTRC1\0\0\0')
    out += struct.pack('<IIII', MAIN, DTCM, frames, seed)
    out += t.main + t.dtcm
    freeze = 0
    toggles = []      # (addr, size, a, b, until)
    sweep = None      # (addr, size, values, index)
    for f in range(frames):
        ranges = []
        flags = 0
        if freeze:
            freeze -= 1
        else:
            r = rnd.random()
            if r < 0.01:
                freeze = rnd.choice([1, 2, 5, 30, 120, 600])
            if rnd.random() < 0.02:
                flags |= 1                       # LAG: evaluated as a repeat
            for _ in range(rnd.randint(0, 6)):
                k = rnd.random()
                a = rnd.choice(alist)
                n = addrs[a]
                if k < 0.30:                     # random byte / word
                    w = rnd.choice([1, 2, 4]) if rnd.random() < 0.5 else n
                    t.write(ranges, a, w, rnd.getrandbits(8 * w))
                elif k < 0.55 and clist:         # sweep a condition's constant
                    if not sweep or sweep[3] >= len(sweep[2]):
                        ca, cn, cv = rnd.choice(clist)
                        sweep = (ca, cn, [cv - 1, cv, cv + 1, cv, cv], 0)
                    ca, cn, vals, i = sweep
                    t.write(ranges, ca, cn, vals[i])
                    sweep = (ca, cn, vals, i + 1)
                elif k < 0.65:                   # toggle A, B, A, B
                    old = t.read(a, n) or 0
                    toggles.append((a, n, old, rnd.getrandbits(8 * n), f + rnd.randint(4, 40)))
                elif k < 0.80 and chains:        # pointer move, then its target
                    base, bn, offs = rnd.choice(chains)
                    p = t.read(base, bn)
                    if p is None:
                        continue
                    if rnd.random() < 0.15:
                        newp = 0x023FFFF0 + rnd.randint(0, 0x1F)   # near the end: short reads
                    else:
                        newp = (p + rnd.choice([-0x40, -4, 4, 0x20, 0x100])) & 0xFFFFFFFF
                    t.write(ranges, base, bn, newp)
                    for o in offs:
                        ta = ((newp & 0x3FFFFF) + o) & 0xFFFFFFFF
                        if in_region(ta, 4):
                            t.write(ranges, ta, rnd.choice([1, 2, 4]), rnd.getrandbits(32))
                elif k < 0.85 and chains:        # a chain's target changes, its pointer doesn't
                    base, bn, offs = rnd.choice(chains)
                    p = t.read(base, bn)
                    if p is None or not offs:
                        continue
                    ta = ((p & 0x3FFFFF) + rnd.choice(offs)) & 0xFFFFFFFF
                    if in_region(ta, 4):
                        t.write(ranges, ta, rnd.choice([1, 2, 4]), rnd.getrandbits(32))
                else:                            # an increment (counters, timers)
                    v = t.read(a, n)
                    if v is not None:
                        t.write(ranges, a, n, v + 1)
            keep = []
            for (a, n, va, vb, until) in toggles:
                t.write(ranges, a, n, va if f % 2 else vb)
                if f < until:
                    keep.append((a, n, va, vb, until))
            toggles = keep
        # coalesce writes per address (last wins), little records
        out += struct.pack('<IBBH', f, flags, 0, len(ranges))
        for a, b in ranges:
            out += struct.pack('<IH', a, len(b)) + b
    sys.stdout.buffer.write(out)


def cmd_scripts(setpath, seed, frames, outdir):
    import os
    s = load_set(setpath)
    rnd = random.Random(int(seed))
    frames = int(frames)
    addrs, _, _ = watched(s)
    alist = sorted(a for a in addrs if in_region(a, 1))
    os.makedirs(outdir, exist_ok=True)
    def w(name, lines):
        with open(os.path.join(outdir, name), 'w') as f:
            f.write(''.join(l + '\n' for l in lines))
    w('1-clean.txt', ['# clean run'])
    w('2-resets.txt', ['%d RESET' % fr for fr in sorted(rnd.sample(range(10, frames), 5))])
    w('3-casual.txt', ['%d HARDCORE 0' % rnd.randint(frames // 10, frames // 2)])
    w('4-pauses.txt', ['%d PAUSE 600' % fr for fr in sorted(rnd.sample(range(10, max(11, frames - 600)), 3))])
    a = rnd.choice(alist) if alist else 0
    w('5-shortread.txt', ['%d SHORTREAD 0x%x %d %d' % (rnd.randint(10, frames // 2), a, addrs.get(a, 1), 60)])
    w('6-award-fail.txt', ['0 FAIL award 3', '%d FAIL award 2' % (frames // 2), '%d FAIL submit 2' % (frames // 3)])
    u = rnd.randint(frames // 4, frames // 2)
    w('7-reload.txt', ['%d UNLOAD' % u, '%d LOAD' % (u + rnd.randint(1, 120))])


def cmd_splits(setpath, seed, n):
    rnd = random.Random(int(seed))
    specs = ['all', 'none', 'mod:2:0', 'mod:2:1']
    for _ in range(int(n)):
        specs.append('random:%d:%d' % (rnd.getrandbits(31), rnd.choice([10, 25, 50, 75, 90])))
    print('\n'.join(specs))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    c, args = sys.argv[1], sys.argv[2:]
    {'session': cmd_session, 'fuzz': cmd_fuzz, 'scripts': cmd_scripts, 'splits': cmd_splits}[c](*args)
