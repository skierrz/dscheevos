#!/usr/bin/env python3
"""Checks one in-game menu round trip from the runner's --trace-io CSV (see README.md).

Usage: check.py IO.csv OPEN CLOSE [--tick ADDR ...] [--report-only]

The menu is open while the SDK's VBlank counter (0x027FFC3C, --trace-word) stands still:
both CPUs sit in the game's IRQ handlers with interrupts off. "Before" is the
sample two frames before it stops, "after" the first sample once it moves again.
Prints one line per check and "RESULT PASS" or "RESULT FAIL".
"""
import csv, sys

TICK_HZ = 33513982 / 64
FRAME_S = 6 * 355 * 263 / 33513982

args = sys.argv[1:]
report_only = "--report-only" in args
args = [a for a in args if a != "--report-only"]
ticks = []
while "--tick" in args:
    i = args.index("--tick")
    ticks.append(int(args[i + 1], 0))
    del args[i:i + 2]
path, open_f, close_f = args[0], int(args[1]), int(args[2])
rows = {int(r["frame"]): r for r in csv.DictReader(open(path))}

def num(r, k):
    return int(r[k], 16) if k.startswith("w") or k in HEX else int(r[k])

HEX = {"ie9", "if9", "ie7", "if7", "tm0cnt9", "tm0val9", "dispcnt_b", "bg3cnt_b", "bldcnt_b",
       "bldalpha_b", "mosaic_b", "powcnt1", "soundcnt"}
vb = "w027ffc3c"
frames = sorted(rows)
# the menu: the first stretch at or after OPEN where the VBlank count stands still
start = next(f for f in frames if f >= open_f and f - 1 in rows and num(rows[f], vb) == num(rows[f - 1], vb))
end = next(f for f in frames if f > start and num(rows[f], vb) != num(rows[f - 1], vb))
before, after = rows[start - 2], rows[end]
frozen = end - start
held = close_f - open_f
fails = []

def check(name, ok, text):
    print("%-26s %s  %s" % (name, "ok  " if ok else "FAIL", text))
    if not ok:
        fails.append(name)

print("menu: game frozen frames %d-%d (%d frames; hotkey at %d, B at %d: %d frames of open/close overhead)"
      % (start, end - 1, frozen, open_f, close_f, frozen - held))

# what run.sh writes 120 frames before each open (--poke). A register that doesn't hold
# that value up to the menu was rewritten by the game, and then proves nothing
POKED = {"bg%d%sofs_b" % (bg, ax): (0x30 if ax == "h" else 0x50) + bg for bg in range(4) for ax in "hv"}
POKED.update({"bldcnt_b": 0x0C48, "bldalpha_b": 0x0808, "bldy_b": 6, "mosaic_b": 0x0101})

def steady(col):
    return all(num(rows[f], col) == POKED[col] for f in range(start - 100, start - 1) if f in rows)

# 2. scroll: every BG the game shows as a text BG must keep its scroll (BGxHOFS/VOFS are write-only)
disp = num(before, "dispcnt_b")
mode = disp & 7
text_bgs = {0: [0, 1, 2, 3], 1: [0, 1, 2], 2: [0, 1], 3: [0, 1, 2], 4: [0, 1], 5: [0, 1]}.get(mode, [])
for bg in range(4):
    shown = bg in text_bgs and disp & (0x100 << bg)
    for ax in ("h", "v"):
        col = "bg%d%sofs_b" % (bg, ax)
        b, a = num(before, col), num(after, col)
        if not steady(col):
            print("%-26s skip  the game rewrote it: %x -> %x" % (col, num(before, col), num(after, col)))
        elif shown:
            check(col, a == b, "%d -> %d (text BG the game shows)" % (b, a))
        else:
            print("%-26s n/a   %d -> %d (not shown as a text BG: mode %d, DISPCNT %08x)" % (col, b, a, mode, disp))

# 3. blending and mosaic
for col in ("bldcnt_b", "bldalpha_b", "bldy_b", "mosaic_b"):
    b, a = num(before, col), num(after, col)
    if not steady(col):
        print("%-26s skip  the game rewrote it: %x -> %x" % (col, num(before, col), num(after, col)))
    else:
        check(col, a == b, "%x -> %x" % (b, a))

# 4. master volume (SOUNDCNT bits 0-6)
b, a = num(before, "soundcnt") & 0x7F, num(after, "soundcnt") & 0x7F
check("master volume", a == b, "%d -> %d" % (b, a))

# 1. time: the game should see the menu as a pause (see README.md): one late VBlank, and the
# OS tick no further ahead than the timer overflow that was left pending (< 2 overflow periods)
dvb = num(after, vb) - num(before, vb)
check("VBlank count", dvb <= 3, "+%d over %d frames (+2 is the two frames before the menu)" % (dvb, frozen + 2))
for t in ticks:
    col = "w%08x" % t
    def tick(r):
        lo = num(r, "tm0val9")
        hi = num(r, col)
        if (num(r, "if9") & 8) and not lo & 0x8000:  # OS_GetTick: an overflow not yet counted
            hi += 1
        return (hi << 16) | lo
    dt = tick(after) - tick(before)
    wall = (frozen + 2) * FRAME_S
    check("OS tick %08x" % t, 0 <= dt < 2 * 65536 + 3 * 8738,
          "+%.0f ms of game time for %.0f ms of wall time (pause: < %.0f ms)"
          % (dt / TICK_HZ * 1000, wall * 1000, (2 * 65536 + 3 * 8738) / TICK_HZ * 1000))

if report_only:
    print("RESULT REPORT (%d differences)" % len(fails))
else:
    print("RESULT %s" % ("FAIL " + ", ".join(fails) if fails else "PASS"))
    sys.exit(1 if fails else 0)
