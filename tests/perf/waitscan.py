#!/usr/bin/env python3
"""Static census of how a DS ROM waits: signatures in the ARM9 binary and its
overlays (decompressed when they are BLZ-compressed).

Counts, per ROM:
  oshalt     NitroSDK OS_Halt (mov r0,#0; mcr p15,0,r0,c7,c0,4; bx lr), the
             shape nds-bootstrap patches for DScheevos (static binary only)
  cp15halt   other "wait for interrupt" mcr's (mcr p15,0,rX,c7,c0,4), i.e.
             custom halts, outside the OS_Halt shape
  swi4/5/6   BIOS IntrWait / VBlankIntrWait / Halt wrappers: Thumb "swi N; bx lr"
             or ARM "swi N<<16; bx lr" (and loose swi instructions, counted apart)
  idle       NitroSDK idle-thread loop: a call to OS_Halt followed by a branch
             back to it (bl OS_Halt; b <bl>), the shape the module accepts
plus the SDK version from the module params and the "[SDK+...]" middleware
tags in the binary.

Usage: waitscan.py ROM... (prints one TSV row per ROM; --json for details)
"""
import json
import re
import struct
import sys


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def blz_decompress(buf):
    """Nintendo's backward LZ (ARM9 static binary / overlays)."""
    end = len(buf)
    hdr, inc = u32(buf, end - 8), u32(buf, end - 4)
    hdr_len, enc_len = hdr >> 24, hdr & 0xFFFFFF
    if hdr_len < 8 or enc_len > end:
        raise ValueError("bad BLZ footer")
    out = bytearray(buf) + bytearray(inc)
    src, dst, lim = end - hdr_len, end + inc, end - enc_len
    while src > lim:
        src -= 1
        flags = out[src]
        for _ in range(8):
            if src <= lim:
                break
            if flags & 0x80:
                src -= 2
                hi, lo = out[src + 1], out[src]
                disp = (((hi & 0x0F) << 8) | lo) + 3
                n = (hi >> 4) + 3
                for _ in range(n):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = out[src]
            flags = (flags << 1) & 0xFF
    return bytes(out)


def arm9_static(rom):
    off, entry, ram, size = struct.unpack_from("<IIII", rom, 0x20)
    a9 = rom[off:off + size]
    i = a9.find(b"\x21\x06\xc0\xde\xde\xc0\x06\x21")
    sdk = None
    if i >= 0x1C:
        p = i - 0x1C
        cse = u32(a9, p + 0x14)
        sdk = u32(a9, p + 0x18)
        if cse:
            cend = cse - ram
            try:
                a9 = blz_decompress(a9[:cend]) + a9[cend:]
            except Exception:
                pass
    return a9, ram, sdk


def overlays(rom):
    ovt, ovtsz = struct.unpack_from("<II", rom, 0x50)
    fat = u32(rom, 0x48)
    out = []
    for k in range(ovtsz // 32):
        e = rom[ovt + k * 32: ovt + k * 32 + 32]
        oid, ram, size, bss, sinit, sinit_end, fid, flags = struct.unpack("<8I", e)
        start, end = struct.unpack_from("<II", rom, fat + fid * 8)
        data = rom[start:end]
        if flags & 0x01000000:          # compressed
            csz = flags & 0xFFFFFF
            try:
                data = blz_decompress(data[:csz])
            except Exception:
                pass
        out.append((oid, ram, data))
    return out


OSHALT = struct.pack("<III", 0xE3A00000, 0xEE070F90, 0xE12FFF1E)


def scan_code(b, base, static):
    r = dict(oshalt=[], cp15halt=[], swi={4: [], 5: [], 6: []}, swiloose={4: 0, 5: 0, 6: 0}, idle=[])
    # ARM, word aligned
    n = len(b) // 4
    words = struct.unpack_from("<%dI" % n, b, 0)
    halts = set()
    for i, w in enumerate(words):
        if (w & 0x0FFF0FFF) == 0x0E070F90 and (w >> 28) != 0xF:   # mcr p15,0,rX,c7,c0,4
            if i >= 1 and i + 1 < n and words[i - 1] == 0xE3A00000 and w == 0xEE070F90 and words[i + 1] == 0xE12FFF1E:
                r["oshalt"].append(base + (i - 1) * 4)
                halts.add(base + (i - 1) * 4)
            else:
                r["cp15halt"].append(base + i * 4)
        if (w & 0xFF00FFFF) == 0xEF000000 and ((w >> 16) & 0xFF) in (4, 5, 6):
            num = (w >> 16) & 0xFF
            if i + 1 < n and words[i + 1] == 0xE12FFF1E:
                r["swi"][num].append(base + i * 4)
            else:
                r["swiloose"][num] += 1
    # NitroSDK idle loop: bl OS_Halt; b back to the bl (ARM)
    if halts:
        for i, w in enumerate(words[:-1]):
            if (w & 0xFF000000) == 0xEB000000:
                off = w & 0xFFFFFF
                if off & 0x800000:
                    off -= 0x1000000
                tgt = base + i * 4 + 8 + off * 4
                nx = words[i + 1]
                if tgt in halts and (nx & 0xFF000000) == 0xEA000000:
                    boff = nx & 0xFFFFFF
                    if boff & 0x800000:
                        boff -= 0x1000000
                    btgt = base + (i + 1) * 4 + 8 + boff * 4
                    if base + i * 4 - 0x20 <= btgt <= base + i * 4:
                        r["idle"].append(base + i * 4)
    # Thumb, halfword aligned: swi N; bx lr
    m = len(b) // 2
    hw = struct.unpack_from("<%dH" % m, b, 0)
    for i, h in enumerate(hw[:-1]):
        if h in (0xDF04, 0xDF05, 0xDF06):
            num = h & 0xFF
            if hw[i + 1] == 0x4770:
                r["swi"][num].append(base + i * 2)
            else:
                r["swiloose"][num] += 1
    return r


TAG = re.compile(rb"\[SDK\+([A-Za-z0-9_\-\. ]+):([^\]\x00]{0,60})\]")


def scan(path):
    rom = open(path, "rb").read()
    code = rom[0x0C:0x10].decode("ascii", "replace")
    title = rom[0:12].rstrip(b"\x00").decode("ascii", "replace")
    unit = rom[0x12]
    a9, ram, sdk = arm9_static(rom)
    res = dict(rom=path.split("/")[-1], code=code, title=title, unitcode=unit,
               sdk=("%08x" % sdk) if sdk is not None else "", tags=set(),
               oshalt=0, cp15halt=0, idle=0, swi={4: 0, 5: 0, 6: 0}, swiloose={4: 0, 5: 0, 6: 0},
               ovl_cp15halt=0, ovl_swi={4: 0, 5: 0, 6: 0}, sites={})
    s = scan_code(a9, ram, True)
    res["oshalt"] = len(s["oshalt"])
    res["cp15halt"] = len(s["cp15halt"])
    res["idle"] = len(s["idle"])
    res["sites"]["static"] = dict(oshalt=[hex(x) for x in s["oshalt"]], cp15halt=[hex(x) for x in s["cp15halt"]],
                                  idle=[hex(x) for x in s["idle"]],
                                  swi={k: [hex(x) for x in v] for k, v in s["swi"].items()})
    for k in (4, 5, 6):
        res["swi"][k] = len(s["swi"][k])
        res["swiloose"][k] = s["swiloose"][k]
    for t in TAG.findall(a9):
        res["tags"].add((t[0] + b":" + t[1]).decode("ascii", "replace"))
    for oid, oram, data in overlays(rom):
        o = scan_code(data, oram, False)
        res["ovl_cp15halt"] += len(o["cp15halt"]) + len(o["oshalt"])
        for k in (4, 5, 6):
            res["ovl_swi"][k] += len(o["swi"][k])
        if o["cp15halt"] or any(o["swi"].values()):
            res["sites"]["ovl%d" % oid] = dict(cp15halt=[hex(x) for x in o["cp15halt"]],
                                               swi={k: [hex(x) for x in v] for k, v in o["swi"].items()})
        for t in TAG.findall(data):
            res["tags"].add((t[0] + b":" + t[1]).decode("ascii", "replace"))
    res["tags"] = sorted(res["tags"])
    return res


def main():
    args = sys.argv[1:]
    js = "--json" in args
    args = [a for a in args if a != "--json"]
    if not args:
        print(__doc__)
        sys.exit(2)
    if not js:
        print("rom\tcode\tsdk\toshalt\tidle\tcp15halt\tswi4\tswi5\tswi6\tloose4/5/6\tovl_cp15\tovl_swi4/5/6\ttags")
    for p in args:
        try:
            r = scan(p)
        except Exception as e:
            print("%s\tERROR %s" % (p.split("/")[-1], e))
            continue
        if js:
            print(json.dumps(r))
        else:
            print("\t".join(str(x) for x in (
                r["rom"], r["code"], r["sdk"], r["oshalt"], r["idle"], r["cp15halt"],
                r["swi"][4], r["swi"][5], r["swi"][6],
                "%d/%d/%d" % (r["swiloose"][4], r["swiloose"][5], r["swiloose"][6]),
                r["ovl_cp15halt"], "%d/%d/%d" % (r["ovl_swi"][4], r["ovl_swi"][5], r["ovl_swi"][6]),
                " ".join(t for t in r["tags"] if not t.startswith("NINTENDO:")) or "-")))


if __name__ == "__main__":
    main()
