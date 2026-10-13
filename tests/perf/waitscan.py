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
  idle       NitroSDK idle-thread loop, ARM or Thumb: a call to OS_Halt followed
             by a branch right back to it (bl OS_Halt; b <bl>), the shape the
             module accepts
  idle_other a call to OS_Halt followed by a short branch further back (the loop
             calls something else too): the module gives these the stock halt
  callers4/5/6  bl/blx calls to those wrappers from the binary and overlays (the
             SDK's own OS_WaitVBlankIntr doesn't use SWI 5: a caller of the SWI 5
             wrapper is the game or middleware waiting in the BIOS)
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
    r = dict(oshalt=[], cp15halt=[], swi={4: [], 5: [], 6: []}, swiloose={4: 0, 5: 0, 6: 0}, idle=[], idle_other=[])
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
                        (r["idle"] if nx == 0xEAFFFFFD else r["idle_other"]).append(base + i * 4)
    # Thumb, halfword aligned: swi N; bx lr
    m = len(b) // 2
    hw = struct.unpack_from("<%dH" % m, b, 0)
    # calls to OS_Halt from Thumb (bl / blx pair) followed by a branch back:
    # "b .-4" right back to the call is the shape the module accepts
    if halts:
        for i in range(m - 2):
            a, c = hw[i], hw[i + 1]
            if (a & 0xF800) == 0xF000 and (c & 0xE800) == 0xE800:
                off = ((a & 0x7FF) << 12) | ((c & 0x7FF) << 1)
                if off & 0x400000:
                    off -= 0x800000
                tgt = base + i * 2 + 4 + off
                if (c & 0xF800) == 0xE800:
                    tgt &= ~3
                if tgt in halts and (hw[i + 2] & 0xF800) == 0xE000:
                    boff = hw[i + 2] & 0x7FF
                    if boff & 0x400:
                        boff -= 0x800
                    btgt = base + (i + 2) * 2 + 4 + boff * 2
                    if base + i * 2 - 0x20 <= btgt <= base + i * 2:
                        (r["idle"] if hw[i + 2] == 0xE7FC else r["idle_other"]).append(base + i * 2)
    for i, h in enumerate(hw[:-1]):
        if h in (0xDF04, 0xDF05, 0xDF06):
            num = h & 0xFF
            if hw[i + 1] == 0x4770:
                r["swi"][num].append(base + i * 2)
            else:
                r["swiloose"][num] += 1
    return r


def branch_targets(b, base):
    """Yield (site, target) for every bl/blx (ARM and Thumb) in code b at base."""
    n = len(b) // 4
    words = struct.unpack_from("<%dI" % n, b, 0)
    for i, w in enumerate(words):
        if (w & 0x0F000000) == 0x0B000000 or (w >> 25) == 0x7D:
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 0x1000000
            t = base + i * 4 + 8 + off * 4
            if (w >> 28) == 0xF:
                t += ((w >> 24) & 1) * 2
            yield base + i * 4, t
    m = len(b) // 2
    hw = struct.unpack_from("<%dH" % m, b, 0)
    for i in range(m - 1):
        a, c = hw[i], hw[i + 1]
        if (a & 0xF800) == 0xF000 and (c & 0xE800) == 0xE800:
            off = ((a & 0x7FF) << 12) | ((c & 0x7FF) << 1)
            if off & 0x400000:
                off -= 0x800000
            t = base + i * 2 + 4 + off
            if (c & 0xF800) == 0xE800:
                t &= ~3
            yield base + i * 2, t


def calls_to(b, base, wrappers):
    """Count calls landing on a wrapper: wrappers maps (lo, hi) -> key, a call
    to any address in [lo, hi] (the SWI and up to 3 setup instructions before
    it) counts."""
    cnt = {k: [] for k in wrappers}
    for site, t in branch_targets(b, base):
        for (lo, hi) in wrappers:
            if lo <= t <= hi:
                cnt[(lo, hi)].append(site)
                break
    return cnt

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
    res["idle_other"] = len(s["idle_other"])
    res["sites"]["static"] = dict(oshalt=[hex(x) for x in s["oshalt"]], cp15halt=[hex(x) for x in s["cp15halt"]],
                                  idle=[hex(x) for x in s["idle"]], idle_other=[hex(x) for x in s["idle_other"]],
                                  swi={k: [hex(x) for x in v] for k, v in s["swi"].items()})
    for k in (4, 5, 6):
        res["swi"][k] = len(s["swi"][k])
        res["swiloose"][k] = s["swiloose"][k]
    wrappers = {}
    for k in (4, 5, 6):
        for a in s["swi"][k]:
            o = a - ram
            th = struct.unpack_from("<H", a9, o)[0] == 0xDF00 + k
            lo = a - (6 if th else 12)
            # don't reach back past the end of the function before (bx lr)
            for x in range(a - (2 if th else 4), lo - 1, -(2 if th else 4)):
                ins = struct.unpack_from("<H" if th else "<I", a9, x - ram)[0]
                if ins == (0x4770 if th else 0xE12FFF1E):
                    lo = x + (2 if th else 4)
                    break
            wrappers[(lo, a)] = k
    ovls = overlays(rom)
    res["callers"] = {4: 0, 5: 0, 6: 0}
    res["sites"]["callers"] = {4: [], 5: [], 6: []}
    for code_b, code_base in [(a9, ram)] + [(d, oram) for _, oram, d in ovls]:
        for key, sites in calls_to(code_b, code_base, wrappers).items():
            res["callers"][wrappers[key]] += len(sites)
            res["sites"]["callers"][wrappers[key]] += [hex(x) for x in sites]
    for t in TAG.findall(a9):
        res["tags"].add((t[0] + b":" + t[1]).decode("ascii", "replace"))
    for oid, oram, data in ovls:
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
        print("rom\tcode\tsdk\toshalt\tidle\tidle_other\tcp15halt\tswi4\tswi5\tswi6\tloose4/5/6\tovl_cp15\tovl_swi4/5/6\tcallers4/5/6\ttags")
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
                r["rom"], r["code"], r["sdk"], r["oshalt"], r["idle"], r["idle_other"], r["cp15halt"],
                r["swi"][4], r["swi"][5], r["swi"][6],
                "%d/%d/%d" % (r["swiloose"][4], r["swiloose"][5], r["swiloose"][6]),
                r["ovl_cp15halt"], "%d/%d/%d" % (r["ovl_swi"][4], r["ovl_swi"][5], r["ovl_swi"][6]),
                "%d/%d/%d" % (r["callers"][4], r["callers"][5], r["callers"][6]),
                " ".join(t for t in r["tags"] if not t.startswith("NINTENDO:")) or "-")))


if __name__ == "__main__":
    main()
