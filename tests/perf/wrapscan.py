#!/usr/bin/env python3
"""Static check S1 for the BIOS-wait gate (docs/idle-coverage/decision.md 2.1): in the
ARM9 binary as loaded, scanning [load address, +0x4000) the way nds-bootstrap does,
the SDK's SVC_WaitVBlankIntr wrapper (Thumb 2200 DF05 4770) must occur exactly once,
and no bl/blx in the ARM9 binary or its overlays may land inside it (+2 or +4).
Usage: wrapscan.py ROM... (one line per ROM, PASS/FAIL)"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import waitscan  # noqa: E402

PAT = struct.pack("<3H", 0x2200, 0xDF05, 0x4770)


def check(path):
    rom = open(path, "rb").read()
    a9, ram, _ = waitscan.arm9_static(rom)
    region = a9[:0x4000]
    hits = []
    i = region.find(PAT)
    while i >= 0:
        if i % 2 == 0:
            hits.append(ram + i)
        i = region.find(PAT, i + 1)
    if len(hits) != 1:
        return False, "wrapper hits %d" % len(hits), hits
    w = hits[0]
    inside = []
    for code, base in [(a9, ram)] + [(d, oram) for _, oram, d in waitscan.overlays(rom)]:
        for site, t in waitscan.branch_targets(code, base):
            if w < t < w + 6:
                inside.append(site)
    if inside:
        return False, "calls into the middle: %s" % [hex(x) for x in inside], hits
    return True, "wrapper at %#x (+%#x)" % (w, w - ram), hits


def main():
    ok = 0
    for p in sys.argv[1:]:
        good, msg, _ = check(p)
        ok += good
        print("%s\t%s\t%s" % ("PASS" if good else "FAIL", os.path.basename(p), msg))
    print("%d of %d pass" % (ok, len(sys.argv) - 1))


if __name__ == "__main__":
    main()
