#!/usr/bin/env python3
"""Convert the runner's binary PPM (P6, 8-bit) screenshots to PNG, stdlib only.
Usage: ppm2png.py IN.ppm OUT.png"""
import struct, sys, zlib

def main(src, dst):
    data = open(src, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:          # magic, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos)
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P6" or fields[3] != b"255":
        sys.exit("%s: not an 8-bit P6 PPM" % src)
    w, h = int(fields[1]), int(fields[2])
    pix = data[pos + 1:pos + 1 + w * h * 3]
    raw = b"".join(b"\0" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(dst, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
