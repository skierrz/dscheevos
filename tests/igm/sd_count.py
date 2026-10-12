#!/usr/bin/env python3
"""SD card traffic between two frames, from the runner's --trace-sd lines.

Usage: sd_count.py runner.log FROM TO
Prints the 512-byte blocks read and written on the SD card (SDMMC CMD17/18
and CMD24/25) from frame FROM up to, not including, frame TO.
"""
import re, sys

log, lo, hi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
frame = -1
rd = wr = 0
for line in open(log, errors="replace"):
    m = re.match(r"\[sd\] frame (\d+) ", line)
    if m:
        frame = int(m.group(1))
        continue
    m = re.match(r"\[sd\] SDMMC cmd ([0-9a-f]{4}) .* blocks (\d+)", line)
    if not m or not lo <= frame < hi:
        continue
    cmd, blocks = int(m.group(1), 16) & 0x3F, int(m.group(2))
    if cmd == 17: rd += 1
    elif cmd == 18: rd += blocks
    elif cmd == 24: wr += 1
    elif cmd == 25: wr += blocks
print("SD frames %d-%d: %d KB read, %d KB written" % (lo, hi - 1, rd // 2, wr // 2))
