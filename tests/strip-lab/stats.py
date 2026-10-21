#!/usr/bin/env python3
"""LAB ONLY: print the strip counters (dscm_strip_stats) from a RAM dump.
usage: stats.py ram.bin dscheevos9.map"""
import re, struct, sys
NAMES = ("arms wakes_done wakes_cut wakes_before mode_fixed skip_late skip_dispcnt skip_busy skip_mode "
         "frames_armed frames_full frames_cut frames_none frames_unarmed renders arms_vblank stand_downs frames_down skip_gamedma").split()
ram = open(sys.argv[1], "rb").read()
m = re.search(r"0x([0-9a-f]{8})\s+dscm_strip_stats\n", open(sys.argv[2]).read())
off = int(m.group(1), 16) & 0x00FFFFFF
vals = struct.unpack_from("<%dI" % len(NAMES), ram, off)
print("strip: " + " ".join("%s %d" % kv for kv in zip(NAMES, vals)))
