#!/usr/bin/env python3
"""Summary of a wait census (tests/perf/census.sh): how the game waits.

Usage: census_report.py DIR [name:FROM-TO ...] [--json]
  DIR holds frames.csv, sites.csv, hot.csv, io.csv. Segments are in intervals
  from the game's start (TO may be empty: to the end).

Per segment: busy and halted share of an interval (1,120,380 cycles at 67 MHz),
halted split into the SDK's OS_Halt / BIOS waits / other CP15 halts, how many
intervals had any OS_Halt time, and a class:
  oshalt   most halted time is in the SDK's OS_Halt (what DScheevos uses today)
  bios     most halted time is a BIOS wait (SWI IntrWait / VBlankIntrWait / Halt)
  cp15     most halted time is another CP15 halt (custom code)
  oshalt-x most halted time is OS_Halt from a caller the module doesn't accept
  busy     halted < 2% of the time: the game polls or never waits
Then the top halt sites, PC hot spots while running, and IO pollers.
"""
import csv
import json
import os
import sys

FRAME = 560190 * 4      # one interval in 133 MHz units (the runner's unit)
KINDS = {"1": "OS_Halt", "2": "cp15", "3": "OS_Halt (other caller)", "104": "SWI4 IntrWait", "105": "SWI5 VBlankIntrWait", "106": "SWI6 Halt",
         "100": "BIOS (no SWI seen)"}


def load(d):
    rows = []
    with open(os.path.join(d, "frames.csv")) as f:
        for r in csv.DictReader(f):
            rows.append({k: (int(v, 16) if k in ("arm9_pc", "wait_kind") else int(v)) for k, v in r.items()})
    return rows


def classify(halted, osh, bios, oth, osx=0):
    if halted < 0.02:
        return "busy"
    m = max(osh, bios, oth, osx)
    return "oshalt" if m == osh else "bios" if m == bios else "oshalt-x" if m == osx else "cp15"


def segment(rows, a, b):
    sel = [r for r in rows if r["frame"] >= a and (b is None or r["frame"] < b)]
    if not sel:
        return None
    n = len(sel)
    tot = lambda k: sum(r[k] for r in sel) / n / FRAME
    busy, halted = tot("busy"), tot("halted")
    osh, bios, oth = tot("halted_oshalt"), tot("halted_bios"), tot("halted_other")
    osx = tot("halted_oshalt_other") if "halted_oshalt_other" in sel[0] else 0
    with_osh = sum(1 for r in sel if r["halted_oshalt"] > 0)
    # per interval class
    pc = {}
    for r in sel:
        c = classify(r["halted"] / FRAME, r["halted_oshalt"], r["halted_bios"], r["halted_other"], r.get("halted_oshalt_other", 0))
        pc[c] = pc.get(c, 0) + 1
    lines = [r["wait_line"] for r in sel] if "wait_line" in sel[0] else []
    by = {L: sum(1 for x in lines if 0 <= x <= L) / n for L in (160, 176, 184, 191)} if lines else {}
    return dict(n=n, busy=busy, halted=halted, oshalt=osh, bios=bios, other=oth, oshalt_other=osx, with_oshalt=with_osh,
                cls=classify(halted, osh, bios, oth, osx), per_interval=pc, waiting_by_line=by)


def main():
    args = sys.argv[1:]
    js = "--json" in args
    args = [a for a in args if a != "--json"]
    if not args:
        print(__doc__)
        sys.exit(2)
    d = args[0]
    rows = load(d)
    segs = [("all", 0, None)]
    for s in args[1:]:
        name, rng = s.split(":")
        a, b = rng.split("-")
        segs.append((name, int(a), int(b) if b else None))
    out = dict(dir=d, segments={})
    for name, a, b in segs:
        out["segments"][name] = segment(rows, a, b)
    sites = []
    if os.path.exists(os.path.join(d, "sites.csv")):
        with open(os.path.join(d, "sites.csv")) as f:
            for r in csv.DictReader(f):
                sites.append((int(r["cycles"]), r["kind"], r["pc"], r["lr"], int(r["halts"])))
    sites.sort(reverse=True)
    allhalt = sum(s[0] for s in sites) or 1
    out["sites"] = [dict(kind=KINDS.get(k, k), pc=pc, lr=lr, halts=h, share=c / allhalt) for c, k, pc, lr, h in sites[:8]]
    hot = {}
    if os.path.exists(os.path.join(d, "hot.csv")):
        with open(os.path.join(d, "hot.csv")) as f:
            for r in csv.DictReader(f):
                k = int(r["pc"], 16) & ~0x3F
                hot[k] = hot.get(k, 0) + int(r["samples"])
    tot = sum(hot.values()) or 1
    out["hot"] = [dict(pc="%08x" % k, share=v / tot) for k, v in sorted(hot.items(), key=lambda x: -x[1])[:8]]
    io = []
    if os.path.exists(os.path.join(d, "io.csv")):
        with open(os.path.join(d, "io.csv")) as f:
            for r in csv.DictReader(f):
                io.append((int(r["reads"]), r["reg"], r["pc"]))
    io.sort(reverse=True)
    n = len(rows) or 1
    regs = {"04000004": "DISPSTAT", "04000006": "VCOUNT", "04000210": "IE", "04000214": "IF"}
    out["io"] = [dict(reg=regs.get(rg, rg), pc=pc, per_interval=c / n) for c, rg, pc in io[:6]]
    if js:
        print(json.dumps(out))
        return
    print("intervals counted: %d" % len(rows))
    for name, s in out["segments"].items():
        if not s:
            print("%s: no intervals" % name)
            continue
        print("%-8s n=%-6d class %-6s busy %5.1f%%  halted %5.1f%% (OS_Halt %5.1f%%, BIOS %5.1f%%, other %5.1f%%)  "
              "intervals with OS_Halt %d (%.0f%%)  per-interval classes %s" % (
                  name, s["n"], s["cls"], 100 * s["busy"], 100 * s["halted"], 100 * s["oshalt"], 100 * s["bios"],
                  100 * s["other"], s["with_oshalt"], 100.0 * s["with_oshalt"] / s["n"],
                  " ".join("%s:%d" % kv for kv in sorted(s["per_interval"].items()))))
    for name, s in out["segments"].items():
        if s and s["waiting_by_line"]:
            print("%-8s OS_Halt from other callers %.1f%%; intervals already waiting at VBlank since line <=160/176/184/191: %s" % (
                name, 100 * s["oshalt_other"], " ".join("%.0f%%" % (100 * v) for v in s["waiting_by_line"].values())))
    print("halt sites (share of halted time):")
    for s in out["sites"]:
        print("  %5.1f%%  %-22s pc %s lr %s  halts %d" % (100 * s["share"], s["kind"], s["pc"], s["lr"], s["halts"]))
    print("hot spots while running (64-byte blocks, share of samples):")
    for h in out["hot"]:
        print("  %5.1f%%  %s" % (100 * h["share"], h["pc"]))
    print("IO pollers (reads per interval):")
    for i in out["io"]:
        print("  %10.1f  %-8s pc %s" % (i["per_interval"], i["reg"], i["pc"]))


if __name__ == "__main__":
    main()
