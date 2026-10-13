#!/usr/bin/env python3
"""Idle budget from a runner --profile-log CSV (tests/perf/idle.sh).

Per VBlank interval, ARM9 cycles at 67 MHz:
  idle      halted outside the BIOS: the game's OS_Halt (idle thread), the time
            DScheevos gets (nds-bootstrap points OS_Halt at the module)
  halted    all halted time, BIOS waits (IntrWait, VBlankIntrWait) included
  busy      executed cycles
The rest of the interval (stall) is the ARM9 waiting without running or
halting (DMA and other bus stalls).

Usage: idle_report.py prof.csv [name:from-to ...] [--need INSTR[,INSTR...]]
  name:from-to  also report intervals [from, to) of the run (e.g. play:3000-6000)
  --need        instructions per frame to evaluate; prints, at CPI 1.3 and 2.0,
                the share of intervals whose idle covers it and whether it fits
                on average over 1 s and 10 s windows
"""
import csv, sys

FRAME = 560190 * 2          # ARM9 cycles per VBlank interval at 67 MHz
CPIS = (1.3, 2.0)


def pct(v, p):
    """Nearest-rank percentile of a sorted list."""
    if not v:
        return 0
    k = max(0, min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1)))))
    return v[k]


def windows(v, w):
    """Means over consecutive windows of w intervals (sliding, step 1)."""
    if len(v) < w:
        return [sum(v) / max(len(v), 1)]
    out, s = [], sum(v[:w])
    out.append(s / w)
    for i in range(w, len(v)):
        s += v[i] - v[i - w]
        out.append(s / w)
    return out


def load(path):
    rows = list(csv.DictReader(open(path)))
    if rows and "halted" not in rows[0]:
        sys.exit("%s: no halted column; needs the dscheevos-idle-budget runner" % path)
    out = []
    for r in rows:
        halted, bios, busy = int(r["halted"]) / 2, int(r["halted_bios"]) / 2, int(r["busy"]) / 2
        out.append({"idle": halted - bios, "halted": halted, "busy": busy, "lag": int(r["lag"])})
    return out


def summary(name, rows, needs):
    n = len(rows)
    if not n:
        print("%s: no intervals" % name)
        return
    idle = [r["idle"] for r in rows]
    s = sorted(idle)
    halted = sorted(r["halted"] for r in rows)
    busy = sum(r["busy"] for r in rows) / n
    stall = sum(FRAME - r["busy"] - r["halted"] for r in rows) / n
    w60, w600 = sorted(windows(idle, 60)), sorted(windows(idle, 600))
    lag = sum(r["lag"] for r in rows)
    print("%s: %d intervals, lag %d (%.1f%%)" % (name, n, lag, 100.0 * lag / n))
    print("  idle (OS_Halt) cycles/interval: median %d  p10 %d  p1 %d  min %d  mean %d  (%.1f%% of a frame mean)"
          % (pct(s, 50), pct(s, 10), pct(s, 1), s[0], sum(s) / n, 100.0 * sum(s) / n / FRAME))
    print("  idle, 1 s windows (60): p10 %d min %d;  10 s windows (600): min %d"
          % (pct(w60, 10), w60[0], w600[0]))
    print("  all halted (BIOS waits too):    median %d  p10 %d  min %d" % (pct(halted, 50), pct(halted, 10), halted[0]))
    print("  busy mean %d (%.1f%%), stalled mean %d (%.1f%%)" % (busy, 100 * busy / FRAME, stall, 100 * stall / FRAME))
    for need in needs:
        for cpi in CPIS:
            c = need * cpi
            ok = sum(1 for v in idle if v >= c)
            print("  need %d instr x CPI %.1f = %d cycles: %.1f%% of intervals cover it; 1 s windows %s (min %d); 10 s windows %s"
                  % (need, cpi, c, 100.0 * ok / n, "fit" if w60[0] >= c else "short %.0f%% of the time" % (100.0 * sum(1 for v in w60 if v < c) / len(w60)),
                     w60[0], "fit" if w600[0] >= c else "short"))
    return {"median": pct(s, 50), "p10": pct(s, 10), "min": s[0], "mean": sum(s) / n}


def main(argv):
    if not argv or argv[0].startswith("-"):
        sys.exit(__doc__)
    rows = load(argv[0])
    needs, segs, i = [], [], 1
    while i < len(argv):
        if argv[i] == "--need":
            needs = [int(x) for x in argv[i + 1].split(",")]
            i += 2
            continue
        name, rng = argv[i].split(":")
        a, b = rng.split("-")
        segs.append((name, int(a), int(b) if b else len(rows)))
        i += 1
    print("cycles at 67 MHz; a VBlank interval is %d" % FRAME)
    summary("whole run", rows, needs)
    for name, a, b in segs:
        summary("%s [%d, %d)" % (name, a, b), rows[a:b], needs)


if __name__ == "__main__":
    main(sys.argv[1:])
