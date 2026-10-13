#!/usr/bin/env python3
"""What DScheevos costs the GAME: its own ARM9 cycles per VBlank interval, achievements off
vs on, from two runner profile logs (--profile-log; columns game, game_irq, ...).

Usage: gamecost.py OFF.csv ON.csv [--skip N] [--json]
  --skip N  leave out the first N intervals of each run (e.g. menus before a race)

"game" = ARM9 cycles with the PC outside the module (IRQ mode included), in 67 MHz cycles.
The delta row compares the two runs interval by interval; the runs drift apart (the game's
own timing changes), so the averages and CPI are the reliable numbers. "Shadow" is the
runner's shadow-cache estimate from the achievements-on run alone: game cycles lost because
the module's code and data evicted the game's cache lines (I- and D-cache), plus misses caused.
"""
import csv
import json
import sys

FRAME = 560190 * 2   # ARM9 cycles per frame at 67 MHz


def load(path, skip):
    rows = list(csv.DictReader(open(path)))
    if rows and "game" not in rows[0]:
        sys.exit("%s: no game columns (runner without the game counters)" % path)
    return rows[skip:]


def pctl(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else 0


def stats(v):
    return {"avg": sum(v) / max(len(v), 1), "median": pctl(v, 0.5), "p90": pctl(v, 0.9),
            "p99": pctl(v, 0.99), "max": max(v) if v else 0}


def main():
    args = sys.argv[1:]
    skip = 0
    as_json = "--json" in args
    args = [a for a in args if a != "--json"]
    if "--skip" in args:
        i = args.index("--skip")
        skip = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    runs = {"off": load(args[0], skip), "on": load(args[1], skip)}
    if not runs["off"] or not runs["on"]:
        sys.exit("no intervals in one of the runs (did the achievements-on run start the module?)")
    col = lambda m, k: [int(r[k]) for r in runs[m]]
    out = {}
    for m in runs:
        g = [(int(r["game"]) + int(r["game_irq"])) / 2 for r in runs[m]]
        ins = col(m, "game_instr")
        n = len(g)
        out[m] = {"intervals": n, "game": stats(g), "game_irq": stats([x / 2 for x in col(m, "game_irq")]),
                  "instr": stats(ins), "cpi": sum(g) / max(sum(ins), 1),
                  "lag": sum(int(r["lag"]) for r in runs[m]),
                  "imiss": sum(col(m, "game_imiss")) / max(n, 1),
                  "dmiss": sum(col(m, "game_dreadmiss")) / max(n, 1),
                  "wb": sum(col(m, "game_writeback")) / max(n, 1)}
    for m in runs:
        if runs[m] and "irqs" in runs[m][0]:
            irqs = col(m, "irqs")
            out[m]["irqs"] = sum(irqs) / max(len(irqs), 1)
            out[m]["game_irq_per_irq"] = out[m]["game_irq"]["avg"] / max(out[m]["irqs"], 1e-9)
            out[m]["irq_max"] = stats([x / 2 for x in col(m, "irq_max")])
    n = min(len(runs["off"]), len(runs["on"]))
    goff = [(int(r["game"]) + int(r["game_irq"])) / 2 for r in runs["off"]]
    gon = [(int(r["game"]) + int(r["game_irq"])) / 2 for r in runs["on"]]
    out["delta"] = stats([gon[i] - goff[i] for i in range(n)])
    out["delta_avg_pct_frame"] = 100 * (out["on"]["game"]["avg"] - out["off"]["game"]["avg"]) / FRAME
    out["cpi_delta_pct"] = 100 * (out["on"]["cpi"] / out["off"]["cpi"] - 1) if out["off"]["cpi"] else 0
    on = runs["on"]
    out["shadow"] = {"total": stats([(int(r["game_iextra"]) + int(r["game_dextra"])) / 2 for r in on]),
                     "icache": stats([int(r["game_iextra"]) / 2 for r in on]),
                     "dcache": stats([int(r["game_dextra"]) / 2 for r in on]),
                     "poll_i": sum(col("on", "poll_i")) / max(len(on), 1),
                     "poll_d": sum(col("on", "poll_d")) / max(len(on), 1),
                     "poll_wb": sum(col("on", "poll_wb")) / max(len(on), 1)}
    if "resumes" in on[0]:
        out["shadow"]["irq_mode"] = stats([int(r["game_extra_irq"]) / 2 for r in on])
        out["shadow"]["resumes"] = sum(col("on", "resumes")) / max(len(on), 1)
    out["shadow_avg_pct_frame"] = 100 * out["shadow"]["total"]["avg"] / FRAME
    if "arm7_busy" in runs["on"][0] and any(col("on", "arm7_busy")):
        out["arm7_busy"] = {m: stats(col(m, "arm7_busy")) for m in runs}
    if as_json:
        print(json.dumps(out, indent=1))
        return
    f = lambda s: "%9.0f %9.0f %9.0f %9.0f" % (s["avg"], s["median"], s["p90"], s["max"])
    print("the game's own ARM9 cycles per interval (67 MHz)       avg    median       p90       max")
    for m in ("off", "on"):
        o = out[m]
        print("  achievements %-3s (%5d intervals, %3d lag)  %s" % (m, o["intervals"], o["lag"], f(o["game"])))
    print("  on - off, interval by interval              %s" % f(out["delta"]))
    print("  on - off average: %+.0f cycles = %+.2f%% of a frame; game CPI %.3f -> %.3f (%+.1f%%)" % (
        out["on"]["game"]["avg"] - out["off"]["game"]["avg"], out["delta_avg_pct_frame"],
        out["off"]["cpi"], out["on"]["cpi"], out["cpi_delta_pct"]))
    if "irqs" in out["on"] and "irqs" in out["off"]:
        print("  IRQ handler runs/interval: off %.0f, on %.0f; game cycles per run off %.0f, on %.0f;"
              " longest run p99 off %.0f, on %.0f, max off %.0f, on %.0f" % (
              out["off"]["irqs"], out["on"]["irqs"], out["off"]["game_irq_per_irq"], out["on"]["game_irq_per_irq"],
              out["off"]["irq_max"]["p99"], out["on"]["irq_max"]["p99"], out["off"]["irq_max"]["max"], out["on"]["irq_max"]["max"]))
    print("  game instructions/interval: off %.0f, on %.0f" % (out["off"]["instr"]["avg"], out["on"]["instr"]["avg"]))
    print("  game cache misses/interval: I %.0f -> %.0f, D read %.0f -> %.0f, write-backs %.0f -> %.0f" % (
        out["off"]["imiss"], out["on"]["imiss"], out["off"]["dmiss"], out["on"]["dmiss"], out["off"]["wb"], out["on"]["wb"]))
    s = out["shadow"]
    print("  shadow cache (on run): cycles the game lost to our cache use")
    print("    I + D                                     %s  (%.2f%% of a frame)" % (f(s["total"]), out["shadow_avg_pct_frame"]))
    print("    I-cache                                   %s" % f(s["icache"]))
    print("    D-cache                                   %s" % f(s["dcache"]))
    print("    misses we caused per interval: I %.0f, D %.0f, dirty write-backs %.0f" % (s["poll_i"], s["poll_d"], s["poll_wb"]))
    if "resumes" in s:
        print("    of it in the game's IRQ mode              %s" % f(s["irq_mode"]))
        print("    the game resumed after module code %.1f times per interval (%.0f cycles lost per resume)" % (
            s["resumes"], s["total"]["avg"] / max(s["resumes"], 1e-9)))
    if "arm7_busy" in out:
        for m in ("off", "on"):
            print("  ARM7 busy cycles/interval (33 MHz), %-3s     %s" % (m, f(out["arm7_busy"][m])))


main()
