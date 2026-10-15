#!/usr/bin/env python3
"""Behaviour-identity gate: compare a DScheevos run against the neutral reference, frame by frame.

Usage: bgate.py REF RUN [--json FILE] [--detail N]
  REF, RUN: run base paths; each has BASE.bg.{meta,snap,frames.csv,io.csv} (runner --bgate BASE.bg) and,
  if present, BASE.spu7 (--spu7), BASE.pcm (--audio-out), BASE.prof.csv (the profile log).
  Both runs must come from the same savestate with the same input (tests/behaviour/run.sh).

Prints PASS/FAIL per category with the first differing frame and address. Never prints memory contents:
memory is compared by 64-bit hashes of 4 KB blocks; addresses and counts only. See README.md.
"""
import array, collections, csv, json, math, os, struct, sys

HDR = struct.Struct('<6I')


def load_meta(base):
    m = {'regions': [], 'rules': [], 'lines': []}
    for line in open(base + '.bg.meta'):
        w = line.split()
        if not w:
            continue
        if w[0] == 'region':
            m['regions'].append(dict(name=w[1], base=int(w[2], 16), size=int(w[3], 16), blk=int(w[4], 16), first=int(w[5])))
        elif w[0] == 'rule':
            m['rules'].append(w[2])
        elif w[0] == 'blocks':
            m['blocks'] = int(w[1])
        else:
            m['lines'].append(line.rstrip())
    return m


def load_snap(base, meta):
    nr, nb = len(meta['rules']), meta['blocks']
    rsz = HDR.size + nr * 20 + nb * 8
    data = open(base + '.bg.snap', 'rb').read()
    recs = {0: {}, 1: {}}
    for off in range(0, len(data) - rsz + 1, rsz):
        magic, kind, key, frame, vcount, flags = HDR.unpack_from(data, off)
        if magic != 0x31524742:
            raise SystemExit('%s: bad record at %d' % (base, off))
        p = off + HDR.size
        rh = struct.unpack_from('<%dQ' % nr, data, p); p += 8 * nr
        rr = struct.unpack_from('<%dQ' % nr, data, p); p += 8 * nr
        rb = struct.unpack_from('<%dI' % nr, data, p); p += 4 * nr
        blk = data[p:p + 8 * nb]
        recs[kind][key] = dict(frame=frame, vcount=vcount, flags=flags, rh=rh, rr=rr, rb=rb, blk=blk)
    return recs


VRAM_BANKS = [('A', 0x20000), ('B', 0x20000), ('C', 0x20000), ('D', 0x20000), ('E', 0x10000), ('F', 0x4000),
              ('G', 0x4000), ('H', 0x8000), ('I', 0x4000)]


def block_name(meta, i):
    for r in meta['regions']:
        n = (r['size'] + r['blk'] - 1) // r['blk']
        if r['first'] <= i < r['first'] + n:
            off = (i - r['first']) * r['blk']
            if r['name'] == 'vram':
                o = off
                for b, sz in VRAM_BANKS:
                    if o < sz:
                        return r['name'], 'VRAM_%s+%05x' % (b, o)
                    o -= sz
            if r['name'] == 'other':
                return r['name'], '%08x' % (r['base'] + off)
            return r['name'], '%08x' % (r['base'] + off)
    return '?', '?'


def region_of(meta, i):
    return block_name(meta, i)[0]


def cmp_state(meta, A, B, kind, game_only=True):
    """Compare snapshot records of one kind. Returns dict with the verdict and details."""
    ra, rb = A[kind], B[kind]
    keys = sorted(set(ra) & set(rb))
    only = sorted(set(ra) ^ set(rb))
    nb = meta['blocks']
    rules = meta['rules']
    diff_keys = []
    per_block = collections.defaultdict(list)   # block -> keys where it differs
    hidden = collections.Counter(); ranges = collections.Counter(); flags = collections.Counter()
    first = None
    for k in keys:
        x, y = ra[k], rb[k]
        for i, r in enumerate(rules):
            if x['rh'][i] != y['rh'][i]:
                hidden[r] += 1
            if x['rr'][i] != y['rr'][i]:
                ranges[r] += 1
        if x['flags'] or y['flags']:
            flags[(x['flags'], y['flags'])] += 1
        if x['blk'] == y['blk']:
            continue
        bx = struct.unpack('<%dQ' % nb, x['blk']); by = struct.unpack('<%dQ' % nb, y['blk'])
        d = [i for i in range(nb) if bx[i] != by[i] and (not game_only or region_of(meta, i) != 'other')]
        dother = [i for i in range(nb) if bx[i] != by[i] and region_of(meta, i) == 'other']
        for i in d + dother:
            per_block[i].append(k)
        if d:
            diff_keys.append(k)
            if first is None:
                first = (k, x['frame'], y['frame'], [block_name(meta, i) for i in d[:8]], len(d))
    # persistence: a block that differs and is equal again at the next compared key healed (transient)
    keyidx = {k: n for n, k in enumerate(keys)}
    persist = {}
    for i, ks in per_block.items():
        runs = []; start = prev = None
        for k in ks:
            if prev is not None and keyidx[k] == keyidx[prev] + 1:
                prev = k; continue
            if start is not None:
                runs.append((start, prev))
            start = prev = k
        if start is not None:
            runs.append((start, prev))
        persist[i] = dict(keys=len(ks), runs=len(runs), longest=max(keyidx[b] - keyidx[a] + 1 for a, b in runs),
                          at_end=ks[-1] == keys[-1])
    game_blocks = {i: v for i, v in persist.items() if region_of(meta, i) != 'other'}
    other_blocks = {i: v for i, v in persist.items() if region_of(meta, i) == 'other'}
    return dict(compared=len(keys), unmatched=len(only), unmatched_first=only[:5], differing=len(diff_keys), first=first,
                blocks=game_blocks, other=other_blocks, hidden=dict(hidden), ranges=dict(ranges), flags=dict(flags),
                ok=not diff_keys and not only)


def load_frames(base):
    rows = list(csv.DictReader(open(base + '.bg.frames.csv')))
    return rows


def cmp_frames(FA, FB):
    n = min(len(FA), len(FB))
    out = {}
    for col, name in (('screen_top', 'top'), ('screen_bottom', 'bottom')):
        d = [int(FA[i]['frame']) for i in range(n) if FA[i][col] != FB[i][col]]
        out[name] = d
    lag = [int(FA[i]['frame']) for i in range(n) if FA[i]['lagflag'] != FB[i]['lagflag']]
    k9 = [int(FA[i]['frame']) for i in range(n) if FA[i]['keys9'] != FB[i]['keys9']]
    k7 = [int(FA[i]['frame']) for i in range(n) if FA[i]['keys7'] != FB[i]['keys7']]
    k9n = [int(FA[i]['frame']) for i in range(n) if FA[i]['keyreads9'] != FB[i]['keyreads9']]
    lagA = sum(int(r['lagflag']) for r in FA[:n]); lagB = sum(int(r['lagflag']) for r in FB[:n])
    return dict(n=n, screens=out, lag=lag, lagA=lagA, lagB=lagB, keys9=k9, keys7=k7, keyreads9=k9n)


def prof_lag(base):
    p = base + '.prof.csv'
    if not os.path.exists(p):
        return None
    return [(int(r['frame']), int(r['lag'])) for r in csv.DictReader(open(p))]


def frame_of(ts, starts):
    # starts: sorted list of (ts7 at frame start, frame)
    import bisect
    i = bisect.bisect_right([s for s, _ in starts], ts) - 1
    return starts[i][1] if i >= 0 else -1


def load_spu(base, starts):
    p = base + '.spu7'
    if not os.path.exists(p):
        return None
    import bisect
    st = [s for s, _ in starts]
    out = []
    for line in open(p):
        w = line.rstrip('\n').split(',')
        if len(w) < 6:
            continue
        ts = int(w[1]); i = bisect.bisect_right(st, ts) - 1
        out.append((starts[i][1] if i >= 0 else -1, ts, w[2], w[3], w[4]))
    return out


def cmp_spu(SA, SB):
    fa = collections.defaultdict(list); fb = collections.defaultdict(list)
    for f, ts, a, s, v in SA:
        fa[f].append((a, s, v))
    for f, ts, a, s, v in SB:
        fb[f].append((a, s, v))
    frames = sorted(set(fa) | set(fb))
    dif = [f for f in frames if fa.get(f) != fb.get(f)]
    first = None
    if dif:
        f = dif[0]; x, y = fa.get(f, []), fb.get(f, [])
        j = next((j for j in range(min(len(x), len(y))) if x[j] != y[j]), min(len(x), len(y)))
        what = 'count %d vs %d' % (len(x), len(y)) if j == min(len(x), len(y)) else \
            'write %d: reg %s/%s %s' % (j, x[j][0], y[j][0], 'value differs' if x[j][0] == y[j][0] else 'register differs')
        first = (f, what)
    # whole-sequence view (frames ignored): first index where register or value differs, and writes that changed frame
    k = min(len(SA), len(SB)); seqdiff = next((i for i in range(k) if SA[i][2:] != SB[i][2:]), None)
    moved = sum(1 for i in range(seqdiff if seqdiff is not None else k) if SA[i][0] != SB[i][0])
    return dict(writes=(len(SA), len(SB)), frames_differing=len(dif), first=first, seq_first=None if seqdiff is None else
                (seqdiff, SA[seqdiff][0], SA[seqdiff][2]), crossed_frame=moved, ok=not dif)


def load_io(base):
    rows = []
    for r in csv.DictReader(open(base + '.bg.io.csv')):
        rows.append((int(r['frame']), r['cpu'], int(r['ts']), r['pc'], r['rw'], r['addr'], r['value'], int(r['count'])))
    return rows


def cmp_ipc(IA, IB):
    """IPC writes (IPCSYNC, FIFO send) per CPU per frame: the commands each CPU gives the other (sound commands go this way)."""
    def per(rows):
        d = collections.defaultdict(list)
        for f, cpu, ts, pc, rw, addr, val, cnt in rows:
            if rw == 'w':
                d[(cpu, f)].extend([(addr, val)] * cnt)
        return d
    a, b = per(IA), per(IB)
    keys = sorted(set(a) | set(b), key=lambda k: (k[1], k[0]))
    dif = [k for k in keys if a.get(k) != b.get(k)]
    return dict(differing=len(dif), first=dif[0] if dif else None, ok=not dif)


def cmp_reads(IA, IB):
    """Timing-sensitive reads by game code (VCOUNT, DISPSTAT, timer counters), per (cpu, pc, register) stream of
    distinct values: where the game first saw a different value. Diagnostic (a different value read is the usual way a
    timing shift becomes a behaviour change)."""
    def per(rows):
        d = collections.defaultdict(list)
        for f, cpu, ts, pc, rw, addr, val, cnt in rows:
            if rw == 'r':
                s = d[(cpu, pc, addr)]
                if not s or s[-1][1] != val:
                    s.append((f, val))
        return d
    a, b = per(IA), per(IB)
    firsts = []
    for k in set(a) | set(b):
        x, y = a.get(k, []), b.get(k, [])
        j = next((j for j in range(min(len(x), len(y))) if x[j][1] != y[j][1]), None)
        if j is None and len(x) != len(y):
            j = min(len(x), len(y))
        if j is not None:
            f = min(x[j][0] if j < len(x) else 1 << 30, y[j][0] if j < len(y) else 1 << 30)
            firsts.append((f, k, j))
    firsts.sort()
    return dict(streams=len(set(a) | set(b)), differing=len(firsts), first=firsts[:8])


def cmp_pcm(base_a, base_b):
    pa, pb = base_a + '.pcm', base_b + '.pcm'
    if not (os.path.exists(pa) and os.path.exists(pb)):
        return None
    x = array.array('h'); x.frombytes(open(pa, 'rb').read())
    y = array.array('h'); y.frombytes(open(pb, 'rb').read())
    m = min(len(x), len(y)); nz = 0; sq = 0; ssq = 0; peak = 0
    for i in range(m):
        d = x[i] - y[i]
        if d:
            nz += 1; sq += d * d
            if abs(d) > peak:
                peak = abs(d)
        ssq += x[i] * x[i]
    db = lambda v: 20 * math.log10(v / 32768) if v > 0 else float('-inf')
    return dict(samples=m, differing=nz, pct=100.0 * nz / max(m, 1), peak=peak, rms_db=db(math.sqrt(sq / max(m, 1))),
                signal_db=db(math.sqrt(ssq / max(m, 1))), lengths=(len(x), len(y)))


def main():
    args = sys.argv[1:]
    jpath = None; detail = 6
    if '--json' in args:
        i = args.index('--json'); jpath = args[i + 1]; del args[i:i + 2]
    if '--detail' in args:
        i = args.index('--detail'); detail = int(args[i + 1]); del args[i:i + 2]
    if len(args) != 2:
        print(__doc__); sys.exit(2)
    ref, run = args
    meta = load_meta(ref); meta_b = load_meta(run)
    if [r['name'] for r in meta['regions']] != [r['name'] for r in meta_b['regions']]:
        raise SystemExit('region layouts differ')
    A, B = load_snap(ref, meta), load_snap(run, meta)
    FA, FB = load_frames(ref), load_frames(run)
    res = {}
    out = []
    say = out.append
    say('behaviour gate: %s (reference) vs %s' % (os.path.basename(ref), os.path.basename(run)))
    for line in meta['lines'] + ['--- run:'] + meta_b['lines']:
        if line.startswith(('idle9', 'info9', 'thread', '--- run')):
            say('  meta ' + line)

    def state_line(tag, s):
        if s['ok']:
            return '%s: PASS (%d records compared, %d differing)' % (tag, s['compared'], s['differing'])
        f = s['first']
        txt = '%s: FAIL (%d of %d records differ, %d unmatched)' % (tag, s['differing'], s['compared'], s['unmatched'])
        if f:
            txt += '; first at key %d (frame %d/%d): %d blocks, e.g. %s' % (f[0], f[1], f[2], f[4], ' '.join('%s:%s' % b for b in f[3][:4]))
        elif s['unmatched']:
            txt += '; unmatched keys %s' % s['unmatched_first']
        return txt

    for kind, tag in ((1, 'state (quiescent instant: the game\'s idle thread runs)'), (0, 'state (frame boundary, line 262 end)')):
        s = cmp_state(meta, A, B, kind)
        res['state_q' if kind else 'state_a'] = s
        say(state_line(tag, s))
        bl = sorted(s['blocks'].items(), key=lambda kv: (-kv[1]['at_end'], -kv[1]['keys']))
        for i, v in bl[:detail]:
            r, a = block_name(meta, i)
            say('    %-6s %-16s differs in %d records (%d stretches, longest %d)%s' % (r, a, v['keys'], v['runs'], v['longest'],
                                                                                    ', still at the end' if v['at_end'] else ''))
        if len(bl) > detail:
            say('    ... %d blocks in all (%d still differing at the end)' % (len(bl), sum(1 for _, v in bl if v['at_end'])))
        if s['other']:
            say('    (not game memory) nds-bootstrap/unused main RAM: %d 64 KB blocks differ, e.g. %s' %
                (len(s['other']), ' '.join(block_name(meta, i)[1] for i in sorted(s['other'])[:4])))
        if s['hidden'] or s['ranges']:
            say('    exclusions: masked bytes differed in ' + ', '.join('%s %d' % kv for kv in sorted(s['hidden'].items())) +
                ' records; exclusion ranges differed in ' + (', '.join('%s %d' % kv for kv in sorted(s['ranges'].items())) or 'none'))
        if s['flags']:
            say('    flags (bit 4/8 thread list not found, 16 SP outside its stack, 32/64 IRQ/SVC stack bounds unknown, 128 idle thread unknown): ' +
                ', '.join('%x/%x x%d' % (a, b, n) for (a, b), n in s['flags'].items()))

    fc = cmp_frames(FA, FB); res['frames'] = fc
    for name in ('top', 'bottom'):
        d = fc['screens'][name]
        say('screen %s: %s' % (name, 'PASS (%d frames)' % fc['n'] if not d else 'FAIL (%d of %d frames differ, first %d)' % (len(d), fc['n'], d[0])))
    pl_a, pl_b = prof_lag(ref), prof_lag(run)
    lagtxt = 'lag (melonDS: frames without a keypad read): %s (%d vs %d)' % ('PASS' if not fc['lag'] else 'FAIL (%d frames differ, first %d)' % (len(fc['lag']), fc['lag'][0]), fc['lagA'], fc['lagB'])
    say(lagtxt)
    if pl_a and pl_b:
        n = min(len(pl_a), len(pl_b)); d = [pl_a[i][0] for i in range(n) if pl_a[i][1] != pl_b[i][1]]
        res['lag_idle'] = dict(differing=len(d), a=sum(v for _, v in pl_a[:n]), b=sum(v for _, v in pl_b[:n]))
        say('lag (VBlank intervals in which the game never reached idle): %s (%d vs %d)' %
            ('PASS' if not d else 'FAIL (%d intervals differ, first frame %d)' % (len(d), d[0]), res['lag_idle']['a'], res['lag_idle']['b']))
    say('input (keypad values the game read, per frame): ARM9 %s; ARM7 %s; ARM9 read counts differ in %d frames (polling, informational)' %
        ('PASS' if not fc['keys9'] else 'FAIL (%d frames, first %d)' % (len(fc['keys9']), fc['keys9'][0]),
         'PASS' if not fc['keys7'] else 'FAIL (%d frames, first %d)' % (len(fc['keys7']), fc['keys7'][0]), len(fc['keyreads9'])))
    starts_a = [(int(r['ts7']), int(r['frame'])) for r in FA]
    starts_b = [(int(r['ts7']), int(r['frame'])) for r in FB]
    SA, SB = load_spu(ref, starts_a), load_spu(run, starts_b)
    if SA is not None and SB is not None:
        sp = cmp_spu(SA, SB); res['spu'] = sp
        say('sound commands (ARM7 SPU register writes, register+value, per frame): %s; writes %d vs %d; sequence %s; writes that moved to another frame %d' %
            ('PASS' if sp['ok'] else 'FAIL (%d frames differ, first %d: %s)' % (sp['frames_differing'], sp['first'][0], sp['first'][1]),
             sp['writes'][0], sp['writes'][1], 'identical' if sp['seq_first'] is None else 'differs from write %d (frame %d, reg %s)' % sp['seq_first'],
             sp['crossed_frame']))
    IA, IB = load_io(ref), load_io(run)
    ipc = cmp_ipc(IA, IB); res['ipc'] = ipc
    say('IPC (IPCSYNC/FIFO-send writes per CPU per frame, incl. the ARM9\'s sound commands): %s' %
        ('PASS' if ipc['ok'] else 'FAIL (%d cpu-frames differ, first ARM%s frame %d)' % (ipc['differing'], ipc['first'][0], ipc['first'][1])))
    rd = cmp_reads(IA, IB); res['reads'] = rd
    say('timing-sensitive reads by game code (VCOUNT/DISPSTAT/timers; diagnostic): %d of %d (cpu, pc, register) streams saw different values' %
        (rd['differing'], rd['streams']))
    for f, (cpu, pc, addr), j in rd['first'][:detail]:
        say('    frame %d: ARM%s pc %s reads %s (value #%d differs)' % (f, cpu, pc, addr, j))
    pc = cmp_pcm(ref, run); res['pcm'] = pc
    if pc:
        say('sound samples (as close as possible): %.3f%% of %d differ, peak |d| %d, diff RMS %.1f dBFS vs signal %.1f%s' %
            (pc['pct'], pc['samples'], pc['peak'], pc['rms_db'], pc['signal_db'], '' if pc['lengths'][0] == pc['lengths'][1] else ' (lengths differ %d/%d)' % pc['lengths']))
    verdict = res['state_q']['ok'] and not fc['screens']['top'] and not fc['screens']['bottom'] and not fc['lag'] and \
        not fc['keys9'] and not fc['keys7'] and (res.get('spu') or {'ok': True})['ok'] and \
        (res.get('lag_idle') is None or res['lag_idle']['differing'] == 0)
    say('VERDICT: %s (game state at the quiescent instant, screens, lag, input, sound commands)' % ('PASS' if verdict else 'FAIL'))
    res['verdict'] = verdict
    print('\n'.join(out))
    if jpath:
        def clean(o):
            if isinstance(o, dict):
                return {str(k): clean(v) for k, v in o.items() if k != 'blk'}
            if isinstance(o, (list, tuple)):
                return [clean(v) for v in o]
            return o
        json.dump(clean(res), open(jpath, 'w'), indent=1, default=str)


if __name__ == '__main__':
    main()
