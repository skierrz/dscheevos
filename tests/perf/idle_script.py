#!/usr/bin/env python3
"""Build a runner input script (--script) from phases, for tests/perf/idle.sh.

Usage: idle_script.py OUT TOTAL PHASE...
  TOTAL  frames the script covers (screenshots every SHOT_EVERY frames, 250)
  PHASE  FROM-TO:KIND, frames counted from the game's first OS_Halt call
    generic[(X;Y)]   menus: A, a touch at (X, Y) (default 128;96), Start, every 120 frames
    generic2[(X;Y)]  menus where touches matter: A, two touches, Start, every 160 frames
    play:ITEM,...    ITEM is KEY@PERIOD/HOLD[+OFFSET] (a press every PERIOD frames) or
                     holdKEY (held for the whole phase). KEY: A B X Y L R U D Le Ri St Se,
                     T(X;Y) (a touch), F(X;Y;DX;DY) (a flick: touch, then 4 steps to +DX,+DY)
Optional: DUMPS_AT=f1,f2 (RAM dumps then), SHOT_EVERY=n.
Example: idle_script.py kirby.txt 6000 "0-1200:generic" "1200-6000:play:holdRi,A@45/8,B@100/6+20"
"""
import os
import sys

KEYS = dict(A=1, B=2, Se=4, St=8, Ri=16, Le=32, U=64, D=128, R=256, L=512, X=1024, Y=2048)


def generic(lines, a, b, kind):
    tx, ty = 128, 96
    if "(" in kind:
        tx, ty = map(int, kind[kind.index("(") + 1:-1].split(";"))
    if kind.startswith("generic2"):
        seq, per = ((0, "1 4"), (50, "TOUCH %d %d 10" % (tx, ty)), (100, "TOUCH %d %d 10" % (tx, ty)), (140, "8 4")), 160
    else:
        seq, per = ((30, "1 4"), (60, "TOUCH %d %d 10" % (tx, ty)), (90, "8 4")), 120
    for k in range(a, b, per):
        for off, cmd in seq:
            if k + off < b:
                lines.append("%d %s" % (k + off, cmd))


def play(lines, a, b, spec):
    for item in spec.split(","):
        if item.startswith("hold"):
            lines.append("%d %d %d" % (a, KEYS[item[4:]], b - a))
            continue
        key, rest = item.split("@")
        per, rest = rest.split("/")
        hold, off = (rest.split("+") + ["0"])[:2]
        for t in range(a + int(off), b, int(per)):
            if key.startswith("F("):
                x, y, dx, dy = map(int, key[2:-1].split(";"))
                lines.append("%d TOUCH %d %d 4" % (t, x, y))
                for i in range(1, 5):
                    lines.append("%d TOUCH %d %d 1" % (t + 3 + i, x + dx * i // 4, y + dy * i // 4))
            elif key.startswith("T("):
                x, y = key[2:-1].split(";")
                lines.append("%d TOUCH %s %s %s" % (t, x, y, hold))
            else:
                lines.append("%d %d %s" % (t, KEYS[key], hold))


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    out, total, lines = argv[0], int(argv[1]), []
    for ph in argv[2:]:
        rng, kind = ph.split(":", 1)
        a, b = map(int, rng.split("-"))
        if kind.startswith("generic"):
            generic(lines, a, b, kind)
        elif kind.startswith("play:"):
            play(lines, a, b, kind[5:])
        else:
            sys.exit("unknown phase kind: " + kind)
    every = int(os.environ.get("SHOT_EVERY", "250"))
    lines += ["%d SHOT" % k for k in range(every, total + 1, every)]
    lines += ["%s DUMP" % d for d in os.environ.get("DUMPS_AT", "").split(",") if d]
    open(out, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
