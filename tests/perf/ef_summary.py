#!/usr/bin/env python3
"""Measurement only (research/optimize/everyframe.md). Joins ef2cov's frames.csv with copyonce.c's
CO_FRAMES file (one replay of the real set) and prints, per game: how many frames the module evaluates
with a fresh snapshot vs the last one again, whether those stale frames are lag frames, and how often
each candidate's watched bytes, events and evaluated state differ from the RetroArch instant (A).

  ef_summary.py frames.csv co_frames.csv [label]

Candidates in CO_FRAMES order: P (module today), PF (module keeping completed captures), F (first idle
capture after VBlank), V (VBlank start). Lag proxy: bit 0 of DTCM+0x3FF8 (the SDK's VBlank check flag)
still set at line 192 = the game's main loop did not enter its VBlank wait during that interval.
"""
import csv, sys

meta = {int(r["v"]): r for r in csv.DictReader(open(sys.argv[1]))}
names = ["P", "PF", "F", "V"]
rows = []
for line in open(sys.argv[2]):
    x = line.strip().split(",")
    if len(x) < 13: continue
    v = int(x[0])
    c = [(int(x[1 + 3 * k]), int(x[2 + 3 * k]), int(x[3 + 3 * k])) for k in range(4)]
    if v in meta: rows.append((v, meta[v], c))
label = sys.argv[3] if len(sys.argv) > 3 else ""
n = len(rows)
def pct(a, b): return "%d (%.1f%%)" % (a, 100.0 * a / b) if b else "0"
fresh = [r for r in rows if r[1]["stage"] == "1"]
stale = [r for r in rows if r[1]["stage"] == "0"]
stale_pf = [r for r in stale if r[1]["pf"] == "1"]
lag = lambda r: int(r[1]["flag3ff8"], 16) & 1
print("== %s: %d frames counted" % (label, n))
print("module snapshot: fresh %s, stale (last snapshot again) %s; of the stale: a completed capture of that frame was discarded %s, no completed capture %s"
      % (pct(len(fresh), n), pct(len(stale), n), pct(len(stale_pf), len(stale)), pct(len(stale) - len(stale_pf), len(stale))))
sl = [r for r in stale if lag(r)]
print("stale frames that are lag frames (main loop never reached its VBlank wait): %s; not lag: %s" % (pct(len(sl), len(stale)), pct(len(stale) - len(sl), len(stale))))
print("all frames that are lag frames: %s" % pct(sum(1 for r in rows if lag(r)), n))
for grp, rs in (("fresh", fresh), ("stale", stale), ("stale+lag", sl), ("stale+not lag", [r for r in stale if not lag(r)])):
    if not rs: continue
    w = sum(1 for r in rs if r[2][0][0]); e = sum(1 for r in rs if r[2][0][1]); s = sum(1 for r in rs if r[2][0][2])
    print("  P on %-14s frames %6d: watched bytes differ from A %s, events differ %s, state differs %s" % (grp, len(rs), pct(w, len(rs)), pct(e, len(rs)), pct(s, len(rs))))
for k, nm in enumerate(names):
    w = sum(1 for r in rows if r[2][k][0]); e = sum(1 for r in rows if r[2][k][1]); s = sum(1 for r in rows if r[2][k][2])
    print("  %-3s vs A (RetroArch instant): watched differ %s, event frames differ %s, state differs %s" % (nm, pct(w, n), pct(e, n), pct(s, n)))
late = [int(r[1]["late"]) for r in rows]
print("  queue lateness (intervals after the VBlank): 0: %d, 1: %d, 2+: %d; F present %s"
      % (late.count(0), late.count(1), sum(1 for l in late if l >= 2), pct(sum(1 for r in rows if r[1]["fpresent"] == "1"), n)))
