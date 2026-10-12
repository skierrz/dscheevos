# Table, dsc_vblank breakdown and gate from count.awk output (see run.sh).
#   python3 report.py <out dir> <sets> [--gate limits.txt]
import bisect, collections, os, subprocess, sys

PHASES = [("mark_vblank", "dsc_vblank, queue empty"),
          ("mark_vblank_queued", "dsc_vblank, frame queued"),
          ("mark_idle", "dsc_idle, 1 frame")]
VBLANK = ("mark_vblank", "mark_vblank_queued")


def functions(elf):
    nm = subprocess.run(["arm-none-eabi-nm", "-n", elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW" and not p[2].startswith("$"):
            syms.setdefault(int(p[0], 16) & ~1, p[2])    # Thumb symbols have bit 0 set
    addrs = sorted(syms)
    return lambda pc: syms[addrs[bisect.bisect_right(addrs, pc) - 1]] if pc >= addrs[0] else "?"


def load(path):
    calls, blocks, info = collections.defaultdict(list), collections.Counter(), []
    for line in open(path):
        p = line.split()
        if p[:1] == ["N"]:
            calls[p[1]].append(int(p[2]))
        elif p[:1] == ["PC"]:
            blocks[(p[1], int(p[2], 16))] += int(p[3])
        elif p[:1] in (["INFO"], ["FAIL"]):
            info.append(line.rstrip())
    return calls, blocks, info


def median(v):
    v = sorted(v)
    return v[len(v) // 2]


def main():
    out, sets = sys.argv[1], sys.argv[2].split()
    limits = {}
    if "--gate" in sys.argv:
        for line in open(sys.argv[sys.argv.index("--gate") + 1]):
            p = line.split("#")[0].split()
            if len(p) == 3:
                limits[(p[0], p[1])] = int(p[2])
    measured, rows = {}, []
    for s in sets:
        calls, blocks, info = load(os.path.join(out, "counts-%s.txt" % s))
        for line in info:
            print("%-8s %s" % (s, line))
        if not calls.get("mark_vblank"):
            sys.exit("%s: no dsc_vblank calls counted" % s)
        if not min(calls["mark_vblank"]):
            sys.exit("%s: dsc_vblank counted as 0 instructions (no OBJD- lines in the qemu log?)" % s)
        for ph, label in PHASES:
            if calls.get(ph):
                rows.append((s, label, len(calls[ph]), median(calls[ph]), max(calls[ph])))
        vb = calls["mark_vblank"] + calls.get("mark_vblank_queued", [])
        measured[(s, "vblank_max")] = max(vb)
        measured[(s, "vblank_median")] = median(vb)
        measured[(s, "idle_median")] = median(calls["mark_idle"]) if calls.get("mark_idle") else 0
        fn = functions(os.path.join(out, "perf-%s.elf" % s))
        agg = collections.Counter()
        for (ph, pc), n in blocks.items():
            if ph in VBLANK:
                agg[fn(pc)] += n
        measured[(s, "breakdown")] = (agg, len(vb))
    print()
    print("%-8s %-26s %6s %10s %10s" % ("set", "call", "calls", "median", "max"))
    for r in rows:
        print("%-8s %-26s %6d %10d %10d" % r)
    for s in sets:
        agg, n = measured[(s, "breakdown")]
        total = sum(agg.values()) or 1
        print("\ndsc_vblank by function, %s set (mean per call, own instructions):" % s)
        top = agg.most_common(12)
        for name, v in top:
            print("  %8d  %5.1f%%  %s" % (v // n, 100.0 * v / total, name))
        rest = total - sum(v for _, v in top)
        if rest > 0:
            print("  %8d  %5.1f%%  (other)" % (rest // n, 100.0 * rest / total))
    if limits:
        print()
        failed = 0
        for (s, metric), limit in sorted(limits.items()):
            if s not in sets:
                continue
            got = measured.get((s, metric))
            if got is None:
                print("GATE %s %s: unknown metric" % (s, metric))
                failed += 1
                continue
            ok = got <= limit
            failed += not ok
            print("GATE %-8s %-14s %10d <= %10d  %s" % (s, metric, got, limit, "ok" if ok else "FAIL"))
        sys.exit(1 if failed else 0)


main()
