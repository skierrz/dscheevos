#!/usr/bin/env python3
"""Badge pack sizes for real RetroAchievements sets (docs/display-badges.md).

usage: badge_sizes.py <dir with patch-*.json or sets-*.json>
Prints a Markdown table: core achievements, distinct badges, pack size with
32x32 slots, and with the optional 64x64 section, plus what the launcher
would download (unlocked badges only, at the 5.7 KB mean measured).
"""
import glob
import json
import os
import sys

SLOT = 544
LARGE = 2080
MEAN_PNG = 5668          # bytes, mean of sampled unlocked RA badges


def pack_size(n, large=False):
    names_end = 32 + 4 * n + 16 * n
    slots = (names_end + 511) & ~511
    return slots + SLOT * n + (LARGE * n if large else 0)


def core_achievements(doc):
    sets = doc.get("Sets") or doc.get("PatchData", {}).get("Sets") or []
    out = []
    for s in sets:
        if s.get("Type") not in (None, "core"):
            continue
        for a in s.get("Achievements", []):
            # Flags 3 = core; the server's "Unknown Emulator" warning entry is synthetic
            if a.get("Flags") == 3 and a.get("ID", 0) < 100000000:
                out.append(a)
    return out


def main():
    rows = []
    seen = set()
    for path in sorted(glob.glob(os.path.join(sys.argv[1], "*.json"))):
        try:
            doc = json.load(open(path, encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if not isinstance(doc, dict) or doc.get("GameId") in seen:
            continue
        ach = core_achievements(doc)
        if not ach:
            continue
        seen.add(doc.get("GameId"))
        badges = {a.get("BadgeName") for a in ach}
        rows.append((doc.get("Title", "?"), len(ach), len(badges)))
    rows.sort(key=lambda r: -r[1])
    print("| Game | Achievements | Badges | Pack (32x32) | + 64x64 | First download |")
    print("|---|---|---|---|---|---|")
    for title, n, b in rows:
        print("| %s | %d | %d | %.0f KB | %.0f KB | %.0f KB |" % (
            title.replace("|", "\\|"), n, b, pack_size(n) / 1024, pack_size(n, True) / 1024, b * MEAN_PNG / 1024))
    if rows:
        ns = sorted(r[1] for r in rows)
        med = ns[len(ns) // 2]
        print("\n%d sets; achievements median %d, max %d; pack median %.0f KB, max %.0f KB; "
              "all packs together %.0f KB" % (len(rows), med, ns[-1], pack_size(med) / 1024,
                                             pack_size(ns[-1]) / 1024, sum(pack_size(r[1]) for r in rows) / 1024))


if __name__ == "__main__":
    main()
