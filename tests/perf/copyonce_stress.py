#!/usr/bin/env python3
"""Companion of copyonce.c (research/optimize/copyonce2.md). Reads the runner's COV1 log and

  stats LOG            per-candidate counts: intervals, no-capture intervals, bytes that differ from A
  gen LOG TEMPLATE OUT build stress sets (stock rcheevos syntax) on the bytes the game really changes between
                       halts in a frame: OUT-diff.json (bytes where a candidate differs from A) and
                       OUT-hot.json (bytes A changes most often; control). TEMPLATE is any real set
                       json (only its header fields are used).
  probe LOG SET        for every address a set reads: how many distinct values the log shows (in A)

Log: see tools/melonDS/tools/dscheevos/headless/copyonce.inc. Addresses: main RAM 0..0x3fffff, DTCM at 0x1000000.
"""
import json, re, struct, sys, collections

NMAIN, NIMG = 0x400000, 0x404000
CAND = ["F1", "F2", "MID", "VB"]


def a2o(a):          # rcheevos address -> offset in the log image
    if a < NMAIN: return a
    if 0x1000000 <= a < 0x1004000: return NMAIN + a - 0x1000000
    return None


def o2a(o):
    return o if o < NMAIN else 0x1000000 + o - NMAIN


def parse(path):
    """yield (frame, ncap, a_runs, cands) ; cands = [None | runs] for F1 F2 MID VB ; runs = [(off, bytes)]"""
    with open(path, "rb") as f:
        buf = f.read()
    if buf[:4] != b"COV1": raise SystemExit("bad log")
    p = 4
    n = len(buf)

    def runs():
        nonlocal p
        (k,) = struct.unpack_from("<I", buf, p); p += 4
        out = []
        for _ in range(k):
            off, ln = struct.unpack_from("<II", buf, p); p += 8
            out.append((off, buf[p:p + ln])); p += ln
        return out

    while p + 8 <= n:
        fr, nc = struct.unpack_from("<II", buf, p); p += 8
        if nc == 0:
            yield fr, 0, None, None
            continue
        a = runs()
        c = []
        for _ in range(4):
            h = buf[p]; p += 1
            c.append(runs() if h == 1 else None)
        yield fr, nc, a, c


def walk(path):
    """yield (frame, ncap, A bytearray (updated), changedA offsets, [diff offsets per candidate or None])"""
    A = bytearray(NIMG)
    for fr, nc, ar, cs in parse(path):
        if nc == 0:
            yield fr, 0, A, None, None
            continue
        chg = []
        for off, b in ar:
            old = A[off:off + len(b)]
            for i in range(len(b)):
                if old[i] != b[i]: chg.append(off + i)
            A[off:off + len(b)] = b
        dif = []
        for c in cs:
            if c is None:
                dif.append(None); continue
            d = []
            for off, b in c:
                for i in range(len(b)):
                    if A[off + i] != b[i]: d.append(off + i)
            dif.append(d)
        yield fr, nc, A, chg, dif


def stats(path, skip=120):
    nocap = n = 0
    chg = collections.Counter()
    dcount = [collections.Counter() for _ in CAND]
    present = [0] * 4
    dframes = [0] * 4
    for fr, nc, A, ch, dif in walk(path):
        if nc == 0:
            nocap += 1; continue
        n += 1
        if n <= skip: continue       # the replay's warm-up (the first record is also A against an all-zero image)
        chg.update(ch)
        for k in range(4):
            if dif[k] is None: continue
            present[k] += 1
            if dif[k]: dframes[k] += 1
            dcount[k].update(dif[k])
    return n, nocap, chg, dcount, present, dframes


def cmd_stats(path):
    n, nocap, chg, dc, present, dframes = stats(path)
    print("intervals with a capture %d, without %d (%.1f%% of %d)" % (n, nocap, 100.0 * nocap / max(1, n + nocap), n + nocap))
    print("bytes A changes in some interval: %d; changing in >=50%% of intervals: %d" % (len(chg), sum(1 for v in chg.values() if v * 2 >= n)))
    for k in range(4):
        print("%-4s copy exists %d, intervals whose full RAM differs from A: %d, distinct bytes ever differing: %d" % (CAND[k], present[k], dframes[k], len(dc[k])))


def distinct_vals(series):
    return [v for v, _ in collections.Counter(series).most_common()]


def pick_words(counter, limit, taken, maxv=None):
    out = []
    for o, v in counter:
        if o >= NMAIN or (o >> 2) in taken: continue
        taken.add(o >> 2); out.append(o)
        if len(out) >= limit: break
    return out


def cmd_gen(path, template, out, per=24, nhot=32, skip=120):
    n, nocap, chg, dc, present, dframes = stats(path, skip)
    n -= skip
    sets = {}
    # one set per copy point: bytes where that copy point differs from A in few intervals (a game variable
    # updated late in the frame, not a buffer rewritten every frame) and that A does not rewrite in most intervals
    for k, name in ((0, "F1"), (1, "F2"), (2, "MID")):
        cand = [(o, v) for o, v in dc[k].most_common() if v <= max(5, n * 3 // 100) and chg[o] * 2 < n]
        sets[name] = pick_words(cand, per, set())
    # control: bytes A changes often (not in most intervals) and no copy point sees differently
    quiet = [(o, v) for o, v in chg.most_common(nhot * 8) if v * 2 < n]
    sets["hot"] = pick_words(quiet, nhot, set())
    want = set(o for v in sets.values() for o in v)
    series = {o: bytearray() for o in want}
    m = 0
    for fr, nc, A, ch, dif in walk(path):
        if nc == 0: continue
        m += 1
        if m <= skip: continue
        for o in want: series[o].append(A[o])
    tpl = json.load(open(template))
    for name, picks in sets.items():
        if not picks: continue
        write_set(tpl, "%s-%s.json" % (out, name), picks, series, name)
        print("%s-%s.json: %d addresses" % (out, name, len(picks)))


def write_set(tpl, path, picks, series, tag):
    ach, lbs = [], []
    nid = {"F1": 900000000, "F2": 910000000, "MID": 920000000}.get(tag, 930000000)
    lid = {"F1": 800000, "F2": 810000, "MID": 820000}.get(tag, 830000)

    def A(t, mem):
        nonlocal nid
        nid += 1
        ach.append({"ID": nid, "MemAddr": mem, "Title": "%s %s %d" % (tag, t, nid), "Description": "stress", "Points": 1, "Author": "x",
                    "Modified": 1, "Created": 1, "BadgeName": "00000", "Flags": 3, "Type": "", "Rarity": 1.0, "RarityHardcore": 1.0,
                    "BadgeURL": "http://x/b.png", "BadgeLockedURL": "http://x/l.png"})

    def L(t, mem, fmt="VALUE", lower=False):
        nonlocal lid
        lid += 1
        lbs.append({"ID": lid, "Mem": mem, "Format": fmt, "LowerIsBetter": lower, "Title": "%s lb %s %d" % (tag, t, lid), "Description": "", "Hidden": False})

    def H(o): return "%06x" % o
    for i, o in enumerate(picks):
        s = series[o]
        vals = distinct_vals(s)
        trans = collections.Counter((s[j - 1], s[j]) for j in range(1, len(s)) if s[j] != s[j - 1])
        tl = [t for t, _ in trans.most_common(3)]
        b = picks[(i + 1) % len(picks)]
        c = picks[(i + 7) % len(picks)]
        a = H(o); bb = H(b); cc = H(c)
        nchg = sum(1 for j in range(1, len(s)) if s[j] != s[j - 1])
        for v in vals[:4]:
            A("eq", "0xH%s=%d" % (a, v))
        for (u, v) in tl:
            A("trans", "d0xH%s=%d_0xH%s=%d" % (a, u, a, v))
        for hn in (2, 5, 20, 100):
            if hn <= max(2, nchg):
                A("chg%d" % hn, "0xH%s!=d0xH%s.%d." % (a, a, hn))
        A("inc3-reset", "0xH%s>d0xH%s.3._R:0xH%s!=d0xH%s" % (a, a, bb, bb))
        A("dec4-reset", "0xH%s<d0xH%s.4._R:0xH%s=%d" % (a, a, bb, vals[0]))
        A("measured", "M:0xH%s!=d0xH%s.30." % (a, a))
        A("measured-and", "0xH%s!=d0xH%s_M:0xH%s>d0xH%s.12." % (cc, cc, a, a))
        A("pause", "P:0xH%s=%d_0xH%s!=d0xH%s.6." % (bb, vals[-1], a, a))
        A("andnext", "N:0xH%s=%d_0xH%s!=d0xH%s.4." % (a, vals[0], bb, bb))
        A("ornext", "O:0xH%s=%d_0xH%s=%d.1._0xH%s!=d0xH%s.8." % (a, vals[0], bb, vals[-1], a, a))
        A("prior", "0xH%s=%d_p0xH%s=%d" % (a, vals[0], a, vals[-1]))
        v1, v2, v3 = (vals + vals + vals)[:3]
        if len(vals) >= 2:
            L("val", "STA:0xH%s=%d::CAN:0xH%s=%d::SUB:0xH%s=%d::VAL:0xH%s" % (a, vals[0], bb, vals[-1], a, vals[1], cc))
        L("chg", "STA:0xH%s!=d0xH%s::CAN:0xH%s=%d::SUB:0xH%s>d0xH%s.8.::VAL:M:0xH%s!=d0xH%s.1000000." % (a, a, bb, vals[-1], a, a, a, a))
        L("sum", "STA:d0xH%s=%d_0xH%s=%d::CAN:0=1::SUB:0xH%s!=d0xH%s.15.::VAL:0xH%s_0xH%s*256" % (a, tl[0][0] if tl else 0, a, tl[0][1] if tl else 0, bb, bb, a, cc))
    rp = "Format:V\nFormatType=VALUE\n\nDisplay:\n" + "".join("@V(0xH%s) " % H(o) for o in picks[:6])
    st = {"Success": True, "GameId": tpl["GameId"], "Title": "stress", "ImageIconUrl": tpl.get("ImageIconUrl"), "RichPresenceGameId": tpl["GameId"],
          "RichPresencePatch": rp, "ConsoleId": tpl["ConsoleId"],
          "Sets": [{"Title": None, "Type": "core", "AchievementSetId": tpl["Sets"][0]["AchievementSetId"], "GameId": tpl["GameId"],
                    "ImageIconUrl": tpl.get("ImageIconUrl"), "Achievements": ach, "Leaderboards": lbs}]}
    json.dump(st, open(path, "w"))


ADDR = re.compile(r"0x([A-Z ]?)([0-9a-f]{2,8})")


def cmd_probe(path, setp):
    d = json.load(open(setp))
    addrs = set()
    for s in d["Sets"]:
        for a in s["Achievements"]: addrs.update(int(m.group(2), 16) for m in ADDR.finditer(a["MemAddr"]))
        for a in s["Leaderboards"]: addrs.update(int(m.group(2), 16) for m in ADDR.finditer(a["Mem"]))
    offs = {a: a2o(a) for a in addrs if a2o(a) is not None}
    vals = {a: collections.Counter() for a in offs}
    n = 0
    for fr, nc, A, ch, dif in walk(path):
        if nc == 0: continue
        n += 1
        for a, o in offs.items(): vals[a][A[o]] += 1
    print("addresses read by the set: %d in RAM image, of %d; intervals %d" % (len(offs), len(addrs), n))
    multi = [a for a in offs if len(vals[a]) > 1]
    print("addresses whose first byte takes more than one value in the run: %d" % len(multi))


WIDTH = {" ": 2, "I": 2, "W": 3, "J": 3, "X": 4, "G": 4, "F": 4, "B": 4}


def static_bytes(setp):
    """every byte a set reads at a fixed address (indirect reads: only their offsets, counted separately)"""
    d = json.load(open(setp))
    direct, indirect = set(), 0
    for s in d["Sets"]:
        for t in [a["MemAddr"] for a in s["Achievements"]] + [a["Mem"] for a in s["Leaderboards"]] + [d.get("RichPresencePatch") or ""]:
            indirect += len(re.findall(r"I:", t))
            for m in ADDR.finditer(t):
                a = int(m.group(2), 16); w = WIDTH.get(m.group(1), 1)
                for i in range(w):
                    o = a2o(a + i)
                    if o is not None: direct.add(o)
    return direct, indirect


def cmd_exposure(path, setp):
    n, nocap, chg, dc, present, dframes = stats(path, 120)
    direct, indirect = static_bytes(setp)
    print("set reads %d bytes at fixed addresses, %d indirect (pointer) reads (their targets move)" % (len(direct), indirect))
    for k in range(4):
        inter = sorted(o for o in dc[k] if o in direct)
        print("%-4s bytes ever differing from A: %6d; of those read by the set at a fixed address: %d%s" %
              (CAND[k], len(dc[k]), len(inter), (" (" + ", ".join("%x" % o2a(o) for o in inter[:8]) + ")") if inter else ""))


if __name__ == "__main__":
    c = sys.argv[1]
    if c == "stats": cmd_stats(sys.argv[2])
    elif c == "gen": cmd_gen(sys.argv[2], sys.argv[3], sys.argv[4])
    elif c == "exposure": cmd_exposure(sys.argv[2], sys.argv[3])
    elif c == "probe": cmd_probe(sys.argv[2], sys.argv[3])
    else: raise SystemExit(__doc__)
