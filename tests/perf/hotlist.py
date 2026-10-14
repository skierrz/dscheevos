#!/usr/bin/env python3
# hotlist.py: the module's hot-code list for the layout firewall (dscheevos/module/hot.ld,
# warm.ld, hot-rodata.ld), chosen from the runner's per-PC profiles of real games.
#
# Usage:
#   hotlist.py write MAP OUTDIR FRAMES:IDLE.csv,VBL.csv[,DMISS.csv] ...
#   hotlist.py check MAP FRAMES:IDLE.csv,VBL.csv ...
# MAP: a module build's dscheevos9.map (any layout: sections are matched by object and name).
# Profiles: one achievements-on run per game (ab.sh/gamecost.sh with EXTRA_ARGS
#   "--profile-idle-pcs IDLE.csv --profile-pcs VBL.csv --profile-idle-dmiss DMISS.csv"), FRAMES
#   the run's VBlank intervals. Everything they record is from the game's first OS_Halt on.
#
# Which code is where (module.ld):
#   hot  (hot.ld)  : called at least once per HOT_RATE frames in some game (entry-PC count):
#                    rcheevos, newlib, libgcc and lwIP first, then our own (net, heap, module, core
#                    last), so an edit to our hot code moves none of the third-party hot code.
#   warm (warm.ld) : executed during play less often (pings, unlocks, rich presence, TCP).
#   mbedTLS is never listed: it runs in handshake bursts, and listing it would make the region
#   the size of the whole TLS stack.
#   hot rodata (hot-rodata.ld): constants with D-cache read misses in play (not mbedTLS).
# check: lists code executed during play that is outside the hot and warm regions (the list is
# stale), using the __hot_start/__warm_end symbols of MAP.
import sys, re, bisect, collections, os

HOT_RATE = 0.05          # calls per frame: once per 20 frames
RODATA_RATE = 0.005      # D read misses per frame
EXCLUDE = re.compile(r"(^|/)tls/")
OURS = ["net/", "heap.o", "fast_strtoul.o", "syscalls.o", "module.o", "dscheevos.o"]   # placed last, in this order
# placed explicitly by module.ld (the wake block) or deliberately cold
FIXED = re.compile(r"^\.(text\.irq|text\.entries|text\.wake|text\.wake\.core|data\.cached|text_cold.*)$")


def parse_map(path):
    """(output section, addr, size, input section, object) for each input section in the map."""
    out, outsec, pending = [], None, None
    for l in open(path):
        l = l.rstrip("\n")
        m = re.match(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", l)
        if m:
            outsec = m.group(1); pending = None; continue
        m = re.match(r"^(\.\S+)$", l)
        if m:
            outsec = m.group(1); pending = None; continue
        if pending is not None:
            m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", l)
            if m:
                out.append((outsec, int(m.group(1), 16), int(m.group(2), 16), pending, m.group(3).strip()))
            pending = None; continue
        m = re.match(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", l)
        if m:
            out.append((outsec, int(m.group(2), 16), int(m.group(3), 16), m.group(1), m.group(4).strip())); continue
        m = re.match(r"^ (\.\S+)$", l)
        if m: pending = m.group(1)
    return [s for s in out if s[1] and s[2]]


def symbols(path):
    syms = {}
    for l in open(path):
        m = re.match(r"^\s+0x([0-9a-f]+)\s+(__\w+)\s*=", l) or re.match(r"^\s+0x([0-9a-f]+)\s+(__\w+)$", l)
        if m: syms[m.group(2)] = int(m.group(1), 16)
    return syms


def objkey(obj):
    """A linker-script file pattern for the object: *libc.a:member.o, */rc/client/rc_client.o, */module.o."""
    m = re.search(r"([^/]+\.a)\(([^)]+)\)$", obj)
    if m: return "*%s:%s" % (m.group(1), m.group(2))
    if obj == "linker stubs": return None
    obj = re.sub(r"^.*?/(rc|lwip|tls|net)/", r"\1/", obj)
    obj = re.sub(r"^.*/", "", obj) if not re.match(r"^(rc|lwip|tls|net)/", obj) else obj
    return "*/" + obj


def ours_rank(obj):
    for i, o in enumerate(OURS):
        if (o.endswith("/") and ("/" + o) in obj) or obj.endswith("/" + o):
            return i + 1
    return 0


def profile(secs, runs):
    """per section: max over games of entry calls/frame and of busiest-PC executions/frame;
    rodata D read misses/frame."""
    text = sorted([s for s in secs if s[0] == ".text"], key=lambda s: s[1])
    data = sorted([s for s in secs if s[0] == ".rodata"], key=lambda s: s[1])
    ts, ds = [s[1] for s in text], [s[1] for s in data]
    entry, busiest, dmiss = collections.defaultdict(float), collections.defaultdict(float), collections.defaultdict(float)

    def find(lst, starts, a):
        i = bisect.bisect_right(starts, a) - 1
        return lst[i] if i >= 0 and a < lst[i][1] + lst[i][2] else None

    for run in runs:
        fr, files = run.split(":", 1); fr = float(fr)
        e, b, d = collections.Counter(), collections.Counter(), collections.Counter()
        for f in files.split(","):
            for l in open(f):
                p = l.strip().split(",")
                if p[0] == "pc":
                    a = int(p[1], 16); s = find(text, ts, a)
                    if s is None: continue
                    if a == s[1]: e[s] += int(p[2])
                    b[s] = max(b[s], int(p[2]))
                elif p[0] == "dmiss_r":
                    s = find(data, ds, int(p[1], 16))
                    if s is not None: d[s] += int(p[2])
        for s in b:
            entry[s] = max(entry[s], e[s] / fr); busiest[s] = max(busiest[s], b[s] / fr)
        for s in d:
            dmiss[s] = max(dmiss[s], d[s] / fr)
    return entry, busiest, dmiss


def pattern(s):
    k = objkey(s[4])
    return None if k is None else "%s(%s)" % (k, s[3])


def write(mapf, outdir, runs):
    secs = parse_map(mapf)
    entry, busiest, dmiss = profile(secs, runs)
    hot, warm = [], []
    for s in busiest:
        if FIXED.match(s[3]) or EXCLUDE.search(s[4]) or pattern(s) is None: continue
        rate = entry[s] if entry[s] > 0 else busiest[s]
        (hot if rate >= HOT_RATE else warm).append(s)
    order = lambda s: (ours_rank(s[4]), s[1])
    hot.sort(key=order); warm.sort(key=order)
    rod = sorted([s for s in dmiss if dmiss[s] >= RODATA_RATE and not EXCLUDE.search(s[4])
                  and ".str" not in s[3] and pattern(s)], key=lambda s: s[1])
    src = "generated by tests/perf/hotlist.py from %d profiled games; regenerate, don't edit" % len(runs)
    for name, lst, what in (("hot.ld", hot, "hot code: called at least once per %d frames in play" % round(1 / HOT_RATE)),
                            ("warm.ld", warm, "warm code: executed in play, less often (network, pings, unlocks)"),
                            ("hot-rodata.ld", rod, "hot constants: D-cache read misses in play")):
        with open(os.path.join(outdir, name), "w") as f:
            f.write("/* %s (%s) */\n" % (what, src))
            for s in lst:
                f.write("%s\n" % pattern(s))
        print("%s: %d sections, 0x%x bytes" % (name, len(lst), sum(s[2] for s in lst)))


def check(mapf, runs):
    secs = parse_map(mapf)
    syms = symbols(mapf)
    lo, hi = syms["__hot_start"], syms["__warm_end"]
    entry, busiest, dmiss = profile(secs, runs)
    bad = 0
    for s in sorted(busiest, key=lambda s: -busiest[s]):
        if FIXED.match(s[3]) or EXCLUDE.search(s[4]): continue
        if s[1] < lo or s[1] >= hi:
            rate = entry[s] if entry[s] > 0 else busiest[s]
            print("outside the hot/warm regions: %-50s %-40s %.3f calls/frame" % (s[3], s[4].split("/")[-1], rate))
            bad += 1
    hs = syms["__hot_end"] - lo
    print("hot region 0x%x bytes, warm 0x%x; %d executed sections outside" % (hs, hi - syms["__hot_end"], bad))
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "write":
        write(sys.argv[2], sys.argv[3], sys.argv[4:])
    elif len(sys.argv) >= 3 and sys.argv[1] == "check":
        sys.exit(check(sys.argv[2], sys.argv[3:]))
    else:
        sys.exit(__doc__ or open(__file__).read().split("\nimport")[0])
