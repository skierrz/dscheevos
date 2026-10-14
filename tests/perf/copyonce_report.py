#!/usr/bin/env python3
"""Runs the copyonce replay (copyonce.c) for a log and prints one compact table (research/optimize/copyonce2.md).

  copyonce_report.py REPLAY LOG SET.json [LABEL]         free-running and one-step, one set
Columns per candidate: frames where watched bytes differ, frames where unlock/leaderboard events differ
(free-running: from the first difference on, one cause can carry on; one-step: each frame starts from A's state),
first differing frame, whether the full event lists (frame numbers and leaderboard values) are equal at the end.
"""
import os, re, subprocess, sys, tempfile

CANDS = ["F1", "F2", "MID", "VB", "CTRL"]


def events(path):
    """{('ach', id): [frame...], ('lb', id): [(type, frame, value)...]} from a client's event list"""
    ev = {}
    for tok in open(path).read().split(";"):
        t = tok.split()
        if len(t) < 3: continue
        fr = int(t[0][1:])
        if t[1] == "ach": ev.setdefault(("ach", int(t[2])), []).append(fr)
        elif t[1] == "lb": ev.setdefault(("lb", int(t[3])), []).append((int(t[2]), fr, " ".join(t[4:])))
    return ev


def compare(base, other):
    """per achievement: unlock frame same/earlier/later/missing/extra; per leaderboard: same attempt sequence (start/fail/submit frames and values) or not"""
    r = dict(ach_same=0, ach_earlier=0, ach_later=0, ach_missing=0, ach_extra=0, lb_same=0, lb_diff=0, lb_only=0)
    for k in set(base) | set(other):
        a, b = base.get(k), other.get(k)
        if k[0] == "ach":
            if a and b:
                if a[0] == b[0]: r["ach_same"] += 1
                elif b[0] < a[0]: r["ach_earlier"] += 1
                else: r["ach_later"] += 1
            elif a: r["ach_missing"] += 1
            else: r["ach_extra"] += 1
        else:
            if a and b:
                r["lb_same" if a == b else "lb_diff"] += 1
            else: r["lb_only"] += 1
    return r


def run(replay, log, setp, mode):
    evp = tempfile.mktemp(prefix="coev")
    os.environ["CO_EVENTS"] = evp
    p = subprocess.run([replay, setp, log, "120", mode], capture_output=True, text=True)
    cmp = {}
    try:
        base = events(evp + ".A.txt")
        for c in CANDS:
            cmp[c] = compare(base, events("%s.%s.txt" % (evp, c)))
    except OSError:
        pass
    for f in [evp + "." + n + ".txt" for n in ("A", "F1", "F2", "MID", "VB", "CTRL")]:
        try: os.unlink(f)
        except OSError: pass
    out = p.stdout
    r = {"mode": mode, "cmp": cmp}
    m = re.search(r"frames replayed (\d+) \(counted (\d+)\)", out); r["counted"] = int(m.group(2)) if m else 0
    m = re.search(r"intervals with no capture .*?: (\d+); intervals with at least one capture: (\d+)", out)
    r["nocap"] = int(m.group(1)) if m else -1
    m = re.search(r"captures per interval .*?mean ([\d.]+), max (\d+), exactly one (\d+)", out)
    r["capmean"] = m.group(1) if m else "?"; r["capmax"] = m.group(2) if m else "?"; r["cap1"] = m.group(3) if m else "?"
    m = re.search(r"^A \(last capture, today\): (\d+) achievement", out, re.M)
    r["Aevents"] = int(m.group(1)) if m else -1
    r["Afirst"] = (re.search(r"^A \(last capture, today\): \d+ [^:]*: first: (.*)$", out, re.M) or [None, ""])[1][:200]
    for c in CANDS:
        name = {"F1": "F1 first capture", "F2": "F2 second capture", "MID": "MID capture", "VB": "VB at VBlank", "CTRL": "CTRL"}[c]
        m = re.search(r"^%s.*?copy exists (\d+)\s+full-RAM-differs (\d+)\s+watched-differs (\d+).*?state-differs (\d+)\s+event-frames-differ (\d+) \(first at (\d+); first state difference at (\d+)\)\s+all events equal at end: (\w+)" % re.escape(name), out, re.M)
        if m:
            r[c] = dict(exists=int(m.group(1)), full=int(m.group(2)), watched=int(m.group(3)), state=int(m.group(4)), evf=int(m.group(5)), first=int(m.group(6)), eq=m.group(8))
        m3 = re.search(r"^  %s.*?rich presence differs in (\d+) frames, leaderboard running value/state differs in (\d+) frames" % re.escape(name), out, re.M)
        if c in r and m3: r[c]["rp"] = int(m3.group(1)); r[c]["lbv"] = int(m3.group(2))
        m2 = re.search(r"^%s.*?: (\d+) achievement/leaderboard events" % re.escape(name if c != "CTRL" else "CTRL"), out, re.M)
        if c in r and m2: r[c]["nev"] = int(m2.group(1))
    return r


def main():
    replay, log, setp = sys.argv[1:4]
    label = sys.argv[4] if len(sys.argv) > 4 else setp
    for mode in ("free", "step"):
        r = run(replay, log, setp, mode)
        print("%s [%s] counted %d, no-capture %d, captures/interval mean %s max %s, A events %d" % (label, mode, r["counted"], r["nocap"], r["capmean"], r["capmax"], r["Aevents"]))
        for c in CANDS[:4]:
            if c in r:
                d = r[c]
                print("   %-4s watched-diff frames %5d  event-diff frames %5d (first %s)  events %5d  equal-at-end %s  rich-presence-diff frames %d  lb-value-diff frames %d" %
                      (c, d["watched"], d["evf"], d["first"] if d["evf"] else "-", d.get("nev", -1), d["eq"], d.get("rp", -1), d.get("lbv", -1)))
        for c in CANDS[:4]:
            if mode == "free" and c in r.get("cmp", {}):   # one-step runs resync every frame: per-id lists are not comparable
                q = r["cmp"][c]
                print("   %-4s per id: unlock frame same %d / earlier %d / later %d / missing %d / extra %d;  leaderboard attempts same %d / different %d / one side only %d" %
                      (c, q["ach_same"], q["ach_earlier"], q["ach_later"], q["ach_missing"], q["ach_extra"], q["lb_same"], q["lb_diff"], q["lb_only"]))
        if mode == "step" and "CTRL" in r: print("   noise floor (A again, resynced): event-diff frames %d" % r["CTRL"]["evf"])


main()
