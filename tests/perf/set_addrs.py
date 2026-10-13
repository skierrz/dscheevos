#!/usr/bin/env python3
"""Absolute ARM9 addresses a RetroAchievements DS set reads directly (for the
runner's --dirty-watch): memrefs in achievements, leaderboards and rich
presence, mapped from RA's DS address space (0x000000 = 0x02000000). Offsets
used after a pointer (AddAddress, "I:") are not absolute and are skipped; the
pointer's own address is kept.
Usage: set_addrs.py SET.json > addrs.txt"""
import json
import re
import sys

REF = re.compile(r"0x([HLMNOPQRSTUVWXYZhlmnopqrstuvwxyzGgIiJjKk ]?)([0-9a-fA-F]+)|f([FfBbHhIiMm])([0-9a-fA-F]+)")


def refs(cond_str):
    out = set()
    for group in re.split(r"[SR]?(?:\$|S)", cond_str):
        pointer = False
        for cond in group.split("_"):
            ops = re.split(r"(?:<=|>=|!=|=|<|>|\*|/|&|\^|%|\+|-)", re.sub(r"^[A-Za-z]:", "", cond), maxsplit=1)
            left = ops[0]
            for part in [left] + ops[1:]:
                for m in REF.finditer(part):
                    a = int(m.group(2) or m.group(4), 16)
                    if not pointer:
                        out.add(0x02000000 + (a & 0xFFFFFF))
            pointer = cond.startswith("I:")
    return out


def main():
    d = json.load(open(sys.argv[1]))
    sets = d.get("Sets") or d.get("PatchData", {}).get("Sets") or []
    addrs = set()
    for s in sets:
        for a in s.get("Achievements", []):
            addrs |= refs(a.get("MemAddr", ""))
        for lb in s.get("Leaderboards", []):
            addrs |= refs(lb.get("Mem", ""))
    rp = d.get("RichPresencePatch") or ""
    for line in rp.splitlines():
        if line.startswith("Display:") or "@" in line or "?" in line:
            addrs |= refs(line)
    for a in sorted(addrs):
        if a < 0x02400000:
            print("%08x" % a)


if __name__ == "__main__":
    main()
