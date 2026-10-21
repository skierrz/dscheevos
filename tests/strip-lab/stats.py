#!/usr/bin/env python3
"""LAB ONLY: print the strip counters (dscm_strip_stats) from a RAM dump.
usage: stats.py ram.bin dscheevos9.map"""
import re, struct, sys
NAMES = ("arms wakes_done wakes_cut wakes_before mode_fixed skip_late skip_dispcnt skip_busy skip_mode "
         "frames_armed frames_full frames_cut frames_none frames_unarmed renders arms_vblank stand_downs frames_down skip_gamedma frames_learning learn_frames picks pick_first pick_score no_place resumes gap_lines frames_resumed relaxed_wakes work_under work_lines game_dispcnt").split()
ram = open(sys.argv[1], "rb").read()
m = re.search(r"0x([0-9a-f]{8})\s+dscm_strip_stats\n", open(sys.argv[2]).read())
off = int(m.group(1), 16) & 0x00FFFFFF
vals = struct.unpack_from("<%dI" % len(NAMES), ram, off)
print("strip: " + " ".join("%s %d" % kv for kv in zip(NAMES, vals)))
# STRIP_OBSERVE builds: per 300 frames, how often a bar would have fit (strict / with handlers allowed)
mm = open(sys.argv[2]).read()
o = re.search(r"0x([0-9a-f]{8})\s+dscm_strip_obs\n", mm)
n = re.search(r"0x([0-9a-f]{8})\s+dscm_strip_obs_n\n", mm)
if o and n:
    cnt = struct.unpack_from("<I", ram, int(n.group(1), 16) & 0xFFFFFF)[0]
    base = int(o.group(1), 16) & 0xFFFFFF
    print("observe: window frames | strict best (line) bottom | handlers-allowed best (line) bottom")
    for i in range(max(0, cnt - 64), cnt):
        w = struct.unpack_from("<7H", ram, base + (i % 64) * 14)
        print("  %3d %4d | %4d (%3d) %4d | %4d (%3d) %4d" % ((i,) + w))
