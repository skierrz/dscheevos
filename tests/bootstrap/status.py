#!/usr/bin/env python3
"""Print the DScheevos module status from a main RAM dump (melonds-headless --dump-ram).

  status.py ram.bin dscheevos9.map [--expect ID ...] [--max-dropped N] [--max-missed N]
      [--expect-list N] [--expect-journal-unsent N] [--expect-notice KIND ...]

ABI v6 (module version 6 and up): also the achievement list, the notices and
the offline journal, found through the module header. Modules whose status is
big enough (the map gives its size) also report their own cost in scanlines:
263 a frame, about 4,260 ARM9 cycles each at 67 MHz.
"""
import argparse
import re
import struct
import sys
import time

def hdr_version(ram):
    return struct.unpack_from('<I', ram, 0xD80004)[0]


# dscheevos_module.h, ABI v6
LIST_MAGIC, NOTICES_MAGIC, JOURNAL_MAGIC = 0x4C435344, 0x4E435344, 0x4A435344
ACH_STATES = ['locked', 'unlocked', 'unlocked_casual', 'disabled']
NOTICE_KINDS = ['none', 'session_start', 'unlock', 'mastery', 'progress', 'leaderboard_started',
                'leaderboard_failed', 'leaderboard_submitted', 'leaderboard_rank', 'offline', 'online']
NOTICE_RING, INDICATOR_MAX, JOURNAL_MAX = 16, 8, 96
LIST_ENTRY = struct.Struct('<IIHBB48s100s16s')     # dscm_list_entry_t, 176 bytes
NOTICE = struct.Struct('<IHHIiI48s48s')            # dscm_notice_t, 116 bytes
INDICATOR = struct.Struct('<I24s')
JOURNAL_HEAD = struct.Struct('<IIII32s33sBBx')
JOURNAL_ENTRY = struct.Struct('<BBBBIiI')


def text(b):
    return b.split(b'\0')[0].decode('utf-8', 'replace')


def ram_offset(ram, address, size):
    """Offset of a cached or uncached main RAM address in the dump, or None."""
    o = (address & 0x00FFFFFF) if (address >> 24) in (0x02, 0x0C) else None
    return o if o is not None and o + size <= len(ram) else None


def read_list(ram, hdr):
    o = ram_offset(ram, hdr[11], 16)
    if not hdr[11] or o is None:
        return None
    magic, gen, count, entry_size = struct.unpack_from('<4I', ram, o)
    if magic != LIST_MAGIC or entry_size != LIST_ENTRY.size or ram_offset(ram, hdr[11], 16 + count * entry_size) is None:
        return None
    entries = []
    for i in range(count):
        id_, t, pts, state, flags, title, desc, measured = LIST_ENTRY.unpack_from(ram, o + 16 + i * entry_size)
        entries.append(dict(id=id_, time=t, points=pts, state=state, flags=flags,
                            title=text(title), description=text(desc), measured=text(measured)))
    return dict(generation=gen, entries=entries)


def read_notices(ram, hdr):
    o = ram_offset(ram, hdr[12], 8 + NOTICE_RING * NOTICE.size + 4 + 2 * INDICATOR_MAX * INDICATOR.size)
    if not hdr[12] or o is None or struct.unpack_from('<I', ram, o)[0] != NOTICES_MAGIC:
        return None
    head = struct.unpack_from('<I', ram, o + 4)[0]
    ring = []
    for seq in range(max(1, head - NOTICE_RING + 1), head + 1):
        n = NOTICE.unpack_from(ram, o + 8 + (seq % NOTICE_RING) * NOTICE.size)
        if n[0] == seq:
            ring.append(dict(seq=seq, kind=n[1], id=n[3], value=n[4], frame=n[5], text=text(n[6]), detail=text(n[7])))
    p = o + 8 + NOTICE_RING * NOTICE.size
    gen = struct.unpack_from('<I', ram, p)[0]
    ind = [INDICATOR.unpack_from(ram, p + 4 + i * INDICATOR.size) for i in range(2 * INDICATOR_MAX)]
    challenges = [(i, text(t)) for i, t in ind[:INDICATOR_MAX] if i]
    trackers = [(i, text(t)) for i, t in ind[INDICATOR_MAX:] if i]
    return dict(head=head, ring=ring, generation=gen, challenges=challenges, trackers=trackers)


def read_journal(ram, hdr):
    o = ram_offset(ram, hdr[13], JOURNAL_HEAD.size + JOURNAL_MAX * JOURNAL_ENTRY.size)
    if not hdr[13] or o is None:
        return None
    magic, size, count, game_id, user, game_hash, time_base, jclock = JOURNAL_HEAD.unpack_from(ram, o)
    if magic != JOURNAL_MAGIC:
        return dict(magic=magic, entries=[])
    entries = [JOURNAL_ENTRY.unpack_from(ram, o + JOURNAL_HEAD.size + i * JOURNAL_ENTRY.size)
               for i in range(min(count, JOURNAL_MAX))]
    entries = [dict(kind=e[0], hardcore=e[1], sent=e[2], id=e[4], score=e[5], time=e[6]) for e in entries if e[0]]
    return dict(magic=magic, game_id=game_id, user=text(user), hash=text(game_hash), time_base=time_base, clock=jclock, entries=entries)


ap = argparse.ArgumentParser()
ap.add_argument('ram')
ap.add_argument('map')
ap.add_argument('--expect', type=int, action='append', default=[], help='achievement ID that must have unlocked')
ap.add_argument('--never', type=int, action='append', default=[], help='achievement ID that must not unlock')
ap.add_argument('--max-dropped', type=int, default=0)
ap.add_argument('--max-missed', type=int, help='most VBlanks the module may have missed (no IPC IRQ)')
ap.add_argument('--expect-sent', type=int, help='unlocks the server must have answered during play')
ap.add_argument('--expect-rejoins', type=int, help='times the module must have rejoined the access point')
ap.add_argument('--expect-list', type=int,
                help='achievements the published list must hold; also checks its unlocked ones against the status (ABI v6)')
ap.add_argument('--expect-journal-unsent', type=int, help='journal entries the server has not answered (ABI v6)')
ap.add_argument('--expect-clock', type=lambda v: int(v, 0), help='DSCM_CLOCK_* the status and journal must record')
ap.add_argument('--expect-journal-utc', action='store_true',
                help='journal times are UTC and within 10 minutes of this host\'s clock (needs an entry)')
ap.add_argument('--max-irq-lines', type=int, help='most scanlines one VBlank IRQ of the module may take')
ap.add_argument('--max-irq-avg', type=float, help='average scanlines the VBlank IRQ of the module may take')
ap.add_argument('--expect-notice', action='append', default=[], metavar='KIND',
                help='a notice of this kind must be in the ring, e.g. unlock, session_start (ABI v6)')
args = ap.parse_args()
bad_kinds = [k for k in args.expect_notice if k.lower() not in NOTICE_KINDS]
if bad_kinds:
    ap.error('unknown notice kind: ' + ', '.join(bad_kinds) + ' (one of ' + ', '.join(NOTICE_KINDS[1:]) + ')')
ram = open(args.ram, 'rb').read()
mp = open(args.map).read()
addr = int(re.search(r'0x([0-9a-f]+)\s+dscm_g_status\b', mp).group(1), 16)
m = re.search(r'\.bss\.dscm_g_status\s+0x[0-9a-f]+\s+0x([0-9a-f]+)', mp)
status_size = int(m.group(1), 16) if m else 0
o = addr & 0x00FFFFFF   # the module may link its data at the 0x0C000000 mirror
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
# self-timing, appended to dscm_status_t after net_rejoins
TIMING = 'timed_frames irq_last irq_max irq_total idle_last idle_max idle_total idle_none idle_cut'.split()
timing = None
if status_size >= (p + 92 - o) + 4 * len(TIMING):
    timing = dict(zip(TIMING, struct.unpack_from(f'<{len(TIMING)}I', ram, p + 92)))
    n = timing['timed_frames']
    irq_avg = timing['irq_total'] / n if n else 0.0
    idle_avg = timing['idle_total'] / n if n else 0.0
    print(f"self-timing over {n} frames, in scanlines of 263 (1 line ~4,260 ARM9 cycles at 67 MHz):")
    print(f"  IRQ  avg {irq_avg:.2f} ({irq_avg / 263:.2%} of a frame), max {timing['irq_max']}, last {timing['irq_last']}")
    print(f"  idle avg {idle_avg:.2f} ({idle_avg / 263:.2%} of a frame), max {timing['idle_max']}, last {timing['idle_last']};"
          f" {timing['idle_none']} frames with no idle pass, {timing['idle_cut']} passes cut by a VBlank")
else:
    print('self-timing: not in this module')
# snapshot at halt, appended after the self-timing
HALT = 'captures captures_stopped frames_stale halts_idle halts_other'.split()
if timing and status_size >= (p + 92 - o) + 4 * (len(TIMING) + len(HALT)):
    halt = dict(zip(HALT, struct.unpack_from(f'<{len(HALT)}I', ram, p + 92 + 4 * len(TIMING))))
    print(f"capture at halt: {halt['captures']} snapshots, {halt['captures_stopped']} stopped for an interrupt,"
          f" {halt['frames_stale']} frames with no snapshot (evaluated with the last)")
    print(f"OS_Halt calls: {halt['halts_idle']} from the idle thread, {halt['halts_other']} from game threads (left alone)")
    if status_size >= (p + 92 - o) + 4 * (len(TIMING) + len(HALT) + 1):
        alias = struct.unpack_from('<I', ram, p + 92 + 4 * (len(TIMING) + len(HALT)))[0]
        print(f"0x0C000000 mirror under the game's MPU (diagnostic; not relied on): {'cached by this game (SDK5)' if alias else 'uncached'}")
# the session's speed-ups (DSCM_CLOCK_*), appended after alias_cached
clock = None
if timing and status_size >= (p + 92 - o) + 4 * (len(TIMING) + len(HALT) + 2):
    clock = struct.unpack_from('<I', ram, p + 92 + 4 * (len(TIMING) + len(HALT) + 1))[0]
    print(f"speed-ups chosen before launch: CPU {'133' if clock & 1 else '67'} MHz, VRAM boost {'on' if clock & 2 else 'off'}")
# module memory (dscm_g_heap, heap.h): not ABI, read by name
mh = re.search(r'0x([0-9a-f]+)\s+dscm_g_heap\b', mp)
if mh:
    HEAP = 'low_base low_top high_base high_top load_low_top load_high_top load_in_use arena lent parse_base parse_top parse_heap load_left wrong_view'.split()
    hp = dict(zip(HEAP, struct.unpack_from(f'<{len(HEAP)}I', ram, int(mh.group(1), 16) & 0x00FFFFFF)))
    for k in 'low_base low_top high_base high_top load_low_top load_high_top arena parse_base parse_top'.split():
        if hp[k] >> 24 == 0x0C:   # addresses in the mirror view
            hp[k] = hp[k] & 0x00FFFFFF | 0x02000000
    zone_end = 0x02E00000 + 0x400 + 256 * 1024     # DSCM_SESSION + HEAP_SESSION_HEAD + HEAP_ARENA_BYTES

    def used(low_top, high_top):
        """bytes from the module base to the heap's high-water: the arena at least"""
        return max(high_top, zone_end) - 0x02D80000
    print(f"memory: image+bss {hp['low_base'] - 0x02D80000}, low heap {hp['low_top'] - hp['low_base']}"
          f" (load {hp['load_low_top'] - hp['low_base']}), high heap {max(hp['high_top'] - zone_end, 0)}"
          f" (load {max(hp['load_high_top'] - zone_end, 0)}), malloc'd at load end {hp['load_in_use']}")
    print(f"  region used from the module base, incl. session header + arena: load {used(hp['load_low_top'], hp['load_high_top'])},"
          f" whole run {used(hp['low_top'], hp['high_top'])} (+12 KB stacks at the top)")
    print(f"  arena {'in the session area' if hp['arena'] else 'on the heap'}, replies lent {hp['lent']},"
          f" parsed set in the arena {hp['parse_top'] - hp['parse_base']}, on the heap {hp['parse_heap']}, left {hp['load_left']}")
hdr = struct.unpack_from('<16I', ram, 0xD80000)
# LAB coverage: the BIOS-wait gate's counters, straight from the wake page (dscm_irq at 0xF80000 - 160)
wk = struct.unpack_from('<16I', ram, 0xF80000 - 160)
print(f"bw gate: waits {wk[7] & 0xFFFF}, overruns {wk[7] >> 16}, lines {wk[6]}, longest unit {wk[10]} lines, units {wk[11]}, blocked by a parked idle pass {wk[12]}; frames {wk[1]}")
print('header magic %08x version %d vblank_count %d' % (hdr[15], hdr[1], hdr[9]))

lst = notices = journal = None
if hdr_version(ram) >= 6:
    lst = read_list(ram, hdr)
    if lst:
        es = lst['entries']
        n_un = sum(e['state'] == 1 for e in es)
        n_soft = sum(e['state'] == 2 for e in es)
        print(f"list: {len(es)} achievements, {n_un} unlocked, {n_soft} unlocked in casual only, generation {lst['generation']}")
        for e in es:
            extra = f" [{e['measured']}]" if e['flags'] & 1 else ''
            extra += ' (challenge)' if e['flags'] & 2 else ''
            print(f"  {e['id']} {ACH_STATES[e['state']] if e['state'] < 4 else e['state']}, {e['points']} pts: {e['title']}{extra}")
    else:
        print('list: none published')
    notices = read_notices(ram, hdr)
    if notices:
        print(f"notices: head {notices['head']}, indicators generation {notices['generation']}")
        for n in notices['ring']:
            kind = NOTICE_KINDS[n['kind']] if n['kind'] < len(NOTICE_KINDS) else n['kind']
            print(f"  {n['seq']} {kind} id {n['id']} value {n['value']} frame {n['frame']}: {n['text']} {n['detail']}".rstrip())
        for name in ('challenges', 'trackers'):
            if notices[name]:
                print(f'  {name}: ' + ', '.join(f'{i} {t}' for i, t in notices[name]))
    else:
        print('notices: missing')
    journal = read_journal(ram, hdr)
    if journal and journal['magic'] == JOURNAL_MAGIC:
        unsent = sum(not e['sent'] for e in journal['entries'])
        print(f"journal: {len(journal['entries'])} entries, {unsent} unsent; game {journal['game_id']}, user {journal['user']}, "
              f"times {'UTC' if journal['time_base'] == 1 else 'console clock (UTC offset unknown)'}, speed-ups 0x{journal['clock']:x}")
        for e in journal['entries']:
            what = 'award' if e['kind'] == 1 else 'leaderboard' if e['kind'] == 2 else e['kind']
            score = f" score {e['score']}" if e['kind'] == 2 else ''
            print(f"  {what} {e['id']}{score} {'hardcore' if e['hardcore'] else 'casual'} at {e['time']}: {'sent' if e['sent'] else 'unsent'}")
    else:
        print('journal: empty (no game loaded)')

fails = []
if mh and hp['wrong_view']:
    fails.append(f"{hp['wrong_view']} heap blocks freed through the wrong view (cached vs mirror, heap.c own_view)")
if hdr[15] != 0x56435344:  # LAB coverage: magic at 0x3C
    fails.append('module not loaded')
if not f[0]:
    fails.append('not active')
if err:
    fails.append(f'error {err}')
if f[4] > args.max_dropped:
    fails.append(f'{f[4]} frames dropped')
if args.max_missed is not None and f[5] > args.max_missed:
    fails.append(f'{f[5]} frames missed, limit {args.max_missed}')
fails += [f'{i} did not unlock' for i in args.expect if i not in unlocked]
if timing and f[0] and f[2] and not timing['timed_frames']:
    fails.append('frames snapshotted, none timed')
if args.max_irq_lines is not None or args.max_irq_avg is not None:
    if not timing:
        fails.append('no self-timing to check')
    else:
        if args.max_irq_lines is not None and timing['irq_max'] > args.max_irq_lines:
            fails.append(f"IRQ took up to {timing['irq_max']} lines, limit {args.max_irq_lines}")
        if args.max_irq_avg is not None and irq_avg > args.max_irq_avg:
            fails.append(f'IRQ took {irq_avg:.2f} lines on average, limit {args.max_irq_avg}')
if args.expect_sent is not None:
    sent_now = struct.unpack_from('<I', ram, p + 80)[0] if hdr_version(ram) >= 4 else 0
    if sent_now < args.expect_sent:
        fails.append(f'{sent_now} unlocks sent, expected {args.expect_sent}')
if args.expect_rejoins is not None and rejoins < args.expect_rejoins:
    fails.append(f'rejoined {rejoins} times, expected {args.expect_rejoins}')
fails += [f'{i} unlocked' for i in args.never if i in unlocked]
if hdr_version(ram) >= 6 and f[0]:
    if not lst:
        fails.append('no achievement list published')
    elif args.expect_list is not None:
        # the list is rebuilt at most every 30 frames: dump well after the last unlock
        es = lst['entries']
        listed = {e['id']: e['state'] for e in es}
        # every unlock this session shows as unlocked in the list
        fails += [f'{i} not unlocked in the list' for i in unlocked if listed.get(i) not in (1, None)]
        fails += [f'{i} not in the list' for i in args.expect if i not in listed]
        n_un = sum(s == 1 for s in listed.values())
        ach_un = struct.unpack_from('<I', ram, p + 60)[0]
        if n_un != ach_un:
            fails.append(f'list has {n_un} unlocked, status {ach_un}')
        if args.expect_list is not None and len(es) != args.expect_list:
            fails.append(f'list has {len(es)} achievements, expected {args.expect_list}')
    if not notices:
        fails.append('no notices')
    elif notices['head'] <= NOTICE_RING and not any(n['seq'] == 1 and n['kind'] == 1 for n in notices['ring']):
        fails.append('no session start notice')   # notice 1, while the ring still holds it
if args.expect_list is not None and (hdr_version(ram) < 6 or not lst):
    fails.append('no achievement list to check')
kinds = {NOTICE_KINDS[n['kind']] for n in notices['ring'] if n['kind'] < len(NOTICE_KINDS)} if notices else set()
fails += [f'no {k.lower()} notice' for k in args.expect_notice if k.lower() not in kinds]
if args.expect_journal_unsent is not None:
    unsent = sum(not e['sent'] for e in journal['entries']) if journal else None
    if unsent != args.expect_journal_unsent:
        fails.append(f'journal has {unsent} unsent entries, expected {args.expect_journal_unsent}')
if args.expect_clock is not None:
    if clock != args.expect_clock:
        fails.append(f'status records speed-ups {clock}, expected {args.expect_clock}')
    if journal and journal.get('magic') == JOURNAL_MAGIC and journal['clock'] != args.expect_clock:
        fails.append(f"journal records speed-ups {journal['clock']}, expected {args.expect_clock}")
if args.expect_journal_utc:
    es = journal['entries'] if journal else []
    if not es:
        fails.append('no journal entries to check the time of')
    elif journal['time_base'] != 1:
        fails.append('journal times are not UTC')
    else:
        off = max(abs(e['time'] - time.time()) for e in es)
        if off > 600:
            fails.append(f'journal times up to {off:.0f} s from UTC')
print('RESULT ' + ('FAIL: ' + '; '.join(fails) if fails else 'PASS'))
sys.exit(1 if fails else 0)
