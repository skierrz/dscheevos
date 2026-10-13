#!/usr/bin/env python3
"""Contact sheet of an idle.sh run's screenshots (f<FRAME>.png, then screen.png),
half size, in frame order, so one image shows what a run reached. Stdlib only.
Usage: idle_sheet.py DIR OUT.png [COLUMNS (12)]"""
import glob
import os
import re
import struct
import sys
import zlib


def read_png(path):
    """8-bit RGB, non-interlaced, filter 0 (what ppm2png.py writes)."""
    d = open(path, "rb").read()
    i, idat = 8, b""
    while i < len(d):
        n, = struct.unpack(">I", d[i:i + 4])
        kind = d[i + 4:i + 8]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", d[i + 8:i + 16])
        elif kind == b"IDAT":
            idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    return w, h, [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    src, out = argv[0], argv[1]
    cols = int(argv[2]) if len(argv) > 2 else 12
    files = sorted(glob.glob(os.path.join(src, "f*.png")), key=lambda p: int(re.findall(r"f(\d+)", os.path.basename(p))[0]))
    if os.path.exists(os.path.join(src, "screen.png")):
        files.append(os.path.join(src, "screen.png"))
    if not files:
        sys.exit("no screenshots in " + src)
    tw, th, gap = 128, 192, 4
    rows_n = (len(files) + cols - 1) // cols
    W, H = cols * (tw + gap), rows_n * (th + gap)
    img = [bytearray(W * 3) for _ in range(H)]
    for idx, p in enumerate(files):
        _, _, rows = read_png(p)
        ox, oy = (idx % cols) * (tw + gap), (idx // cols) * (th + gap)
        for y in range(th):
            src_row = rows[y * 2]
            for x in range(tw):
                img[oy + y][(ox + x) * 3:(ox + x) * 3 + 3] = src_row[x * 6:x * 6 + 3]
    raw = b"".join(b"\0" + bytes(r) for r in img)
    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    print("%d screenshots: %s" % (len(files), " ".join(os.path.basename(f) for f in files)))


if __name__ == "__main__":
    main(sys.argv[1:])
