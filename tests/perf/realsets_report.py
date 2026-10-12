#!/usr/bin/env python3
"""Table for tests/perf/realsets.sh: per set, instructions per frame for
dsc_idle (one frame evaluated through the core), dsc_capture (the snapshot) and
rc_client_do_frame alone, and what that is in frames of ARM9 time.

  realsets_report.py <counts.tsv> <sets dir>

The first counted frame is left out (one-time setup). Cycles assume a CPI of
1.3 to 2.0 (qemu counts instructions, not cycles: no cache or wait states).
A frame is 1,117,000 cycles at 67 MHz (DS mode) and 2,234,000 at 133 MHz.
"""
import glob
import json
import os
import re
import sys

FRAME67, FRAME133 = 1_117_000, 2_234_000


def stats(xs):
    xs = sorted(xs)
    if not xs:
        return 0, 0
    med = xs[len(xs) // 2]
    p95 = xs[min(len(xs) - 1, int(round(0.95 * (len(xs) - 1))))]
    return med, p95


def main():
    tsv, sets = sys.argv[1], sys.argv[2]
    rows = {}
    for line in open(tsv):
        gid, variant, mode, ram, out = line.rstrip("\n").split("\t", 4)
        phases = [int(n) for n in re.findall(r"PHASE \d+ (\d+)", out)]
        r = rows.setdefault((variant, gid), {"ram": ram or "random", "ok": True, "info": ""})
        if "DONE" not in out:
            r["ok"] = False
        d = re.search(r"INFO digest ([0-9a-f]+)", out)
        r[mode + "_digest"] = d.group(1) if d else None
        for what, used, top, mx in re.findall(r"INFO heap (\w+) used (\d+) top (\d+) max (\d+)", out):
            r[mode + "_heap_" + what] = (int(used), int(top), int(mx))
        if mode == "core":
            r["capture"] = phases[0::2][1:]
            r["idle"] = phases[1::2][1:]
            m = re.search(r"evaluated (\d+) repeated \d+ dropped (\d+)", out)
            a = re.search(r"core addresses (\d+)", out)
            r["info"] = "addresses %s, evaluated %s, dropped %s" % (
                a.group(1) if a else "?", m.group(1) if m else "?", m.group(2) if m else "?")
        else:
            r["rc"] = phases[1:]

    titles = {}
    for f in glob.glob(os.path.join(sets, "sets-*.json")):
        gid = os.path.basename(f).split("-")[1]
        try:
            titles[gid] = json.load(open(f)).get("Title", "?")
        except ValueError:
            titles[gid] = "?"

    for variant in sorted({v for v, _ in rows}, key=lambda v: v != "stock"):
        keyed = [(gid, r) for (v, gid), r in rows.items() if v == variant]
        keyed.sort(key=lambda kv: -stats(kv[1].get("idle", []))[0])
        print("\n## %s build: instructions per frame (median / p95), %s RAM" % (
            variant, ", ".join(sorted({r["ram"] for _, r in keyed}))))
        print()
        print("| Set | game id | dsc_idle med | p95 | dsc_capture med | rc_client_do_frame med | p95 "
              "| idle+capture, frames of ARM9 @67 MHz (CPI 1.3-2.0) | @133 MHz | notes |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
        for gid, r in keyed:
            im, ip = stats(r.get("idle", []))
            cm, _ = stats(r.get("capture", []))
            rm, rp = stats(r.get("rc", []))
            per = im + cm
            lo67, hi67 = per * 1.3 / FRAME67, per * 2.0 / FRAME67
            lo133, hi133 = per * 1.3 / FRAME133, per * 2.0 / FRAME133
            note = r["info"] + ("" if r["ok"] else "; RUN FAILED")
            print("| %s | %s | %s | %s | %s | %s | %s | %.2f-%.2f | %.2f-%.2f | %s |" % (
                titles.get(gid, "?").replace("|", "/")[:40], gid, f"{im:,}", f"{ip:,}", f"{cm:,}", f"{rm:,}", f"{rp:,}",
                lo67, hi67, lo133, hi133, note))

        print("\n### %s build: core heap (newlib, bytes) and evaluation digest\n" % variant)
        print("Core heap = core run minus rc_client alone, same set: bytes in use after load, "
              "and the heap's high-water mark (highest heap break).\n")
        print("| Set | game id | core heap after load | core high-water | rcheevos after load | digest core | digest rc | match |")
        print("|---|---:|---:|---:|---:|---|---|---|")
        for gid, r in keyed:
            cl, rl = r.get("core_heap_load"), r.get("rc_heap_load")
            ce, re_ = r.get("core_heap_end"), r.get("rc_heap_end")
            cs, rs = r.get("core_heap_start"), r.get("rc_heap_start")
            if cl and rl and ce and re_ and cs and rs:
                load = f"{(cl[0] - cs[0]) - (rl[0] - rs[0]):,}"
                high = f"{ce[2] - re_[2]:,}"
                rcl = f"{rl[0] - rs[0]:,}"
            else:
                load = high = rcl = "?"
            dc, dr = r.get("core_digest"), r.get("rc_digest")
            print("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
                titles.get(gid, "?").replace("|", "/")[:40], gid, load, high, rcl, dc, dr,
                "yes" if dc and dc == dr else "**NO**"))


if __name__ == "__main__":
    main()
