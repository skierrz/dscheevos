#!/usr/bin/env python3
"""Summarise the M1 display survey (melonds-headless --survey output).

usage: summarize.py RUNDIR [--gameplay gameplay.tsv] [--dumps DIR] [--json OUT]

RUNDIR holds, per title, SLUG.json, SLUG.frames.csv.gz and titles.tsv (slug,
ROM name) from run_survey.sh. gameplay.tsv (slug, yes/no/partial, note) is the
hand-made screenshot verdict. --dumps writes each title's IRQ handler bytes
(0x80 from the pointer at DTCM+3FFC) as SLUG.irq.bin plus a variants.tsv.
Prints the markdown tables used in docs/display-research/survey.md.

Strip geometry: N = 32 lines, margin M = 4. Window = lines 261-262 and 0..N+M-1
plus one (0-35). R16 band = within +-2 lines of N or N+M: lines 30-38.
"""
import csv
import gzip
import hashlib
import json
import os
import statistics
import sys

N, M = 32, 4
R16_LO, R16_HI = N - 2, N + M + 2
BOOT_FRAMES = 120          # IE writes in the first 2 s count as boot-time
FIVE_MIN = 18000


def b2(off, size):
    return off <= 2 < off + size


def load_csv(path):
    with gzip.open(path, "rt") as f:
        return list(csv.DictReader(f))


def stretches(flags, minlen=30, skip_start=True):
    out, run, start = [], 0, 0
    for i, v in enumerate(flags + [False]):
        if v:
            if run == 0:
                start = i
            run += 1
        else:
            if run >= minlen and not (skip_start and start == 0):
                out.append((start, run))
            run = 0
    return out


def lit_mask(code, ptr):
    """Mask literal-pool words referenced by PC-relative LDRs inside the dump."""
    try:
        import capstone
    except ImportError:
        return code
    thumb = ptr & 1
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
    md.detail = True
    base = ptr & ~1
    masked = bytearray(code)
    for ins in md.disasm(bytes(code), base):
        if ins.mnemonic.startswith("ldr") and "[pc" in ins.op_str:
            for op in ins.operands:
                if op.type == capstone.arm.ARM_OP_MEM and op.mem.base == capstone.arm.ARM_REG_PC:
                    pcv = (ins.address + (4 if thumb else 8)) & ~3
                    tgt = pcv + op.mem.disp
                    o = tgt - base
                    if 0 <= o <= len(code) - 4:
                        masked[o:o + 4] = b"\0\0\0\0"
    return bytes(masked)


def analyse(rundir, slug, gameplay):
    j = json.load(open(os.path.join(rundir, slug + ".json")))
    rows = load_csv(os.path.join(rundir, slug + ".frames.csv.gz"))
    frames = j["frames"]
    r = {"slug": slug, "frames": frames, "gameplay": gameplay.get(slug, ("?", ""))}
    # DISPCNT
    dA = [d for d in j["dispcnt"] if d["eng"] == "A"]
    dB = [d for d in j["dispcnt"] if d["eng"] == "B"]
    per = lambda v: v / max(frames, 1)
    r["A_w_pf"] = per(sum(d["n"] for d in dA if d["op"] == "w"))
    r["A_r_pf"] = per(sum(d["n"] for d in dA if d["op"] == "r"))
    r["A_buckets_w"] = [per(sum(d[k] for d in dA if d["op"] == "w")) for k in ("b0_35", "b36_191", "b192_260", "b261_262")]
    r["A_buckets_r"] = [per(sum(d[k] for d in dA if d["op"] == "r")) for k in ("b0_35", "b36_191", "b192_260", "b261_262")]
    r["A_win_rmw"] = sum(d["win_rmw"] for d in dA if d["op"] == "w")
    r["A_win_rmwmode"] = sum(d["win_rmw_mode"] for d in dA if d["op"] == "w")
    r["A_win_abs"] = sum(d["win_abs"] for d in dA if d["op"] == "w")
    r["A_win_lo"] = sum(d["win"] for d in dA if d["op"] == "w" and not b2(d["off"], d["size"]))
    r["A_win_reads_b2"] = sum(d["win"] for d in dA if d["op"] == "r" and b2(d["off"], d["size"]))
    r["A_win_reads_b2_frames"] = None
    r["A_bare_win"] = sum(b["win"] for b in j.get("bare_reads", []))
    r["A_bare_win_pcs"] = sorted({b["pc"] for b in j.get("bare_reads", []) if b["win"]})
    r["A_win_abs_pcs"] = sorted({d["pc"] for d in dA if d["op"] == "w" and (d["win_abs"] or d["win_rmw_mode"])})
    r["A_mode_outvbl"] = sum(d["modechg_outvbl"] for d in dA)
    r["A_mode_chg"] = sum(d["modechg"] for d in dA)
    r["B_win_w"] = sum(d["win"] for d in dB if d["op"] == "w")
    r["B_win_r"] = sum(d["win"] for d in dB if d["op"] == "r")
    r["B_mode_outvbl"] = sum(d["modechg_outvbl"] for d in dB)
    # window events: frames with a byte-2 read in the window (from the capped list)
    # IE
    ev = j["ie_abs_events"]
    r["ie_abs"] = j["ie_abs"]
    r["ie_abs_boot"] = sum(1 for e in ev if e["frame"] < BOOT_FRAMES)
    r["ie_abs_later"] = j["ie_abs"] - r["ie_abs_boot"]
    r["ie_abs_later_pcs"] = sorted({e["pc"] for e in ev if e["frame"] >= BOOT_FRAMES})
    r["ie_abs_later_frames"] = len({e["frame"] for e in ev if e["frame"] >= BOOT_FRAMES})
    r["ie_writes"] = j["ie_writes"]
    # raster IRQs
    r["hbl_frames"] = j["hbl_frames"]
    r["vc_frames"] = j["vc_frames"]
    vcl = j["vc_lines"]
    r["vc_top_lines"] = sorted(range(263), key=lambda i: -vcl[i])[:4]
    r["vc_top_lines"] = [i for i in r["vc_top_lines"] if vcl[i]]
    hbl = j["hbl_lines"]
    r["hbl_any_r16"] = sum(hbl[R16_LO:R16_HI + 1])
    # display register writes: raster classification
    raster, dma_raster, r16, r16_keys = 0, 0, 0, []
    early_hist = [0] * 41
    for k in j["regwrites"]:
        lines = k["lines"]
        for i in range(41):
            early_hist[i] += lines[i]
        n = k["n"]
        vis = sum(lines[:192])
        is_dma = k["ctx"].startswith("dma")
        is_irq = k["ctx"] == "irq"
        best = max(sum(lines[i:i + 3]) for i in range(0, 190))
        locked = k["frames"] >= 30 and n >= 30 and vis and best >= 0.8 * n
        band = sum(lines[R16_LO:R16_HI + 1])
        if is_dma:
            if vis:
                dma_raster += 1
            continue
        if (is_irq and vis) or locked:
            raster += 1
            if band >= 5:
                r16 += 1
                r16_keys.append(f"{k['addr']}@{k['pc']}/{k['ctx']}:{band}")
    r["raster_keys"] = raster
    r["dma_raster_keys"] = dma_raster
    r["r16_keys"] = r16_keys
    r["early_hist"] = early_hist
    # capture / FIFO / DMA mode 4
    r["cap_en"] = j["capture_enables"]
    r["cap_fifo"] = j["capture_fifo"]
    r["fifo_writes"] = j["fifo_writes"]
    r["dma4"] = j["dma_mode4"]
    r["cap_srcs"] = sorted({w["capcnt"] for w in j["capture_writes"]})[:4]
    # per frame: fades, black frames
    fade, black, black16, poweroff = [], [], [], []
    for row in rows:
        try:
            dA_ = int(row["dispA"], 16)
            br = int(row["brightA"], 16)
            pw = int(row["powcnt1"], 16)
        except (ValueError, KeyError):
            continue
        fac = min(br & 0x1F, 16)
        fmode = (br >> 14) & 3
        f12 = fmode in (1, 2) and fac >= 12
        fade.append(f12)
        blk = bool(dA_ & 0x80) or ((dA_ >> 16) & 3) == 0 or ((dA_ >> 8) & 0x1F) == 0
        black.append(blk)
        black16.append(blk or (fmode in (1, 2) and fac >= 16))
        poweroff.append(not (pw & 2))
    nf = max(len(rows), 1)
    r["fade_frames_pct"] = 100.0 * sum(fade) / nf
    r["fade_events_5min"] = sum(1 for i in range(1, len(fade)) if fade[i] and not fade[i - 1]) * FIVE_MIN / nf
    st = stretches(black)
    st16 = stretches(black16)
    r["black_stretches_5min"] = len(st) * FIVE_MIN / nf
    r["black16_stretches_5min"] = len(st16) * FIVE_MIN / nf
    r["black_longest"] = max((s[1] for s in st), default=0)
    r["black16_longest"] = max((s[1] for s in st16), default=0)
    # IRQ handler
    hs = j["irq_handler"]
    last = hs[-1] if hs else None
    r["irq_ptrs"] = sorted({h["ptr"] for h in hs if h["frame"] >= 600 or h is last})
    if last and last["code"]:
        code = bytes.fromhex(last["code"])
        ptr = int(last["ptr"], 16)
        r["irq_ptr"] = last["ptr"]
        r["irq_dtcm"] = last["dtcm"]
        r["irq_code"] = code
        r["irq_raw"] = hashlib.sha1(code).hexdigest()[:10]
        r["irq_norm"] = hashlib.sha1(lit_mask(code, ptr)).hexdigest()[:10]
        r["irq_stable"] = len({h["ptr"] for h in hs if h["code"]}) == 1 and len({h["code"] for h in hs if h["code"]}) == 1
    else:
        r["irq_ptr"] = None
    return r


def verdict(r, ie_hook=False):
    g = r["gameplay"][0]
    reasons = []
    if r["cap_fifo"] or r["dma4"] or r["fifo_writes"]:
        reasons.append("FIFO capture/DMA mode 4")
    if r["A_win_abs"] or r["A_win_rmwmode"]:
        reasons.append(f"DISPCNT A non-RMW write in window x{r['A_win_abs'] + r['A_win_rmwmode']}")
    if r["ie_abs_later"] and not ie_hook:
        reasons.append(f"absolute IE writes x{r['ie_abs_later']}")
    if r["r16_keys"]:
        reasons.append(f"raster writes at lines {R16_LO}-{R16_HI}")
    if reasons:
        return "no", "; ".join(reasons)
    if g != "yes":
        return "unknown", "no gameplay reached" if g == "no" else "gameplay only partly reached"
    return "yes", ""


def main():
    args = sys.argv[1:]
    rundir = args[0]
    gp_path = args[args.index("--gameplay") + 1] if "--gameplay" in args else None
    dumps = args[args.index("--dumps") + 1] if "--dumps" in args else None
    outjson = args[args.index("--json") + 1] if "--json" in args else None
    gameplay = {}
    if gp_path and os.path.exists(gp_path):
        for line in open(gp_path):
            if line.strip() and not line.startswith("#"):
                p = line.rstrip("\n").split("\t")
                gameplay[p[0]] = (p[1], p[2] if len(p) > 2 else "")
    titles = []
    for line in open(os.path.join(rundir, "titles.tsv")):
        s, name = line.rstrip("\n").split("\t", 1)
        if os.path.exists(os.path.join(rundir, s + ".json")) and s not in [t[0] for t in titles]:
            titles.append((s, name))
    res = []
    for s, name in titles:
        r = analyse(rundir, s, gameplay)
        r["name"] = name
        r["verdict"], r["why"] = verdict(r)
        r["verdict_iehook"], r["why_iehook"] = verdict(r, ie_hook=True)
        res.append(r)
    # IRQ variants
    variants = {}
    for r in res:
        if r.get("irq_ptr"):
            variants.setdefault(r["irq_norm"], []).append(r)
    vid = {h: f"V{i + 1}" for i, h in enumerate(sorted(variants, key=lambda h: -len(variants[h])))}
    for r in res:
        r["irq_var"] = vid.get(r.get("irq_norm"), "-")
    if dumps:
        os.makedirs(dumps, exist_ok=True)
        with open(os.path.join(dumps, "variants.tsv"), "w") as f:
            f.write("variant\traw_sha1\tnorm_sha1\tptr\tdtcm\tslug\n")
            for r in res:
                if r.get("irq_ptr"):
                    open(os.path.join(dumps, r["slug"] + ".irq.bin"), "wb").write(r["irq_code"])
                    f.write(f"{r['irq_var']}\t{r['irq_raw']}\t{r['irq_norm']}\t{r['irq_ptr']}\t{r['irq_dtcm']}\t{r['slug']}\n")
    # tables
    short = lambda n: n.split(" (")[0][:34]
    print("| Title | Gameplay | DISPCNT A w/r per frame | A in window: byte-2 writes RMW / RMW-mode / abs; low-half writes | A byte-2 reads in window (bare) | A mode change outside VBlank | IE abs writes boot / later | HBlank IRQ frames | VCOUNT IRQ frames (lines) | raster keys (R16 hits) | HBlank-DMA keys | capture en / FIFO / DMA4 | fade>=12 % frames | black >=30f per 5 min (incl. fade 16) | IRQ handler |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in res:
        print(f"| {short(r['name'])} | {r['gameplay'][0]} | {r['A_w_pf']:.1f} / {r['A_r_pf']:.1f} | {r['A_win_rmw']} / {r['A_win_rmwmode']} / {r['A_win_abs']}; {r['A_win_lo']} | "
              f"{r['A_win_reads_b2']} ({r['A_bare_win']}) | {r['A_mode_outvbl']} | {r['ie_abs_boot']} / {r['ie_abs_later']} | {r['hbl_frames']} | "
              f"{r['vc_frames']} ({','.join(map(str, r['vc_top_lines']))}) | {r['raster_keys']} ({len(r['r16_keys'])}) | {r['dma_raster_keys']} | "
              f"{r['cap_en']} / {r['cap_fifo'] + r['fifo_writes']} / {r['dma4']} | {r['fade_frames_pct']:.1f} | {r['black_stretches_5min']:.1f} ({r['black16_stretches_5min']:.1f}) | {r['irq_var']} {r.get('irq_ptr') or ''} |")
    print()
    print("| Title | P2 allow list | Reason | If the IE hook re-ORs bits 28-31 on every IE write |")
    print("|---|---|---|---|")
    for r in res:
        print(f"| {short(r['name'])} | {r['verdict']} | {r['why']} | {r['verdict_iehook']} {('(' + r['why_iehook'] + ')') if r['why_iehook'] else ''} |")
    n = len(res)
    cnt = lambda key, v: sum(1 for r in res if r[key] == v)
    print()
    print(f"titles {n}; reached gameplay {sum(1 for r in res if r['gameplay'][0] == 'yes')}")
    for key in ("verdict", "verdict_iehook"):
        y, no, u = cnt(key, "yes"), cnt(key, "no"), cnt(key, "unknown")
        print(f"{key}: yes {y} ({100 * y / n:.0f}%), no {no}, unknown {u}; of known {y}/{y + no} = {100 * y / max(y + no, 1):.0f}%")
    gp = [r for r in res if r["gameplay"][0] == "yes"]
    med = statistics.median([r["black_stretches_5min"] for r in res]) if res else 0
    med16 = statistics.median([r["black16_stretches_5min"] for r in res]) if res else 0
    medg = statistics.median([r["black_stretches_5min"] for r in gp]) if gp else 0
    print(f"M3 black stretches/5 min: median all {med:.2f} (incl. fade-16 {med16:.2f}); median gameplay titles {medg:.2f}")
    print(f"IRQ handler variants (normalised): {len(variants)}; sizes " + ", ".join(f"{vid[h]}={len(v)}" for h, v in sorted(variants.items(), key=lambda x: -len(x[1]))))
    for h, v in sorted(variants.items(), key=lambda x: -len(x[1])):
        print(f"  {vid[h]}: " + ", ".join(short(r['name']) for r in v))
    rawv = {r["irq_raw"] for r in res if r.get("irq_ptr")}
    print(f"IRQ handler variants (raw bytes): {len(rawv)}")
    for r in res:
        if r["r16_keys"] or r["ie_abs_later"] or r["A_win_abs"] or r["A_win_rmwmode"] or r["A_bare_win_pcs"]:
            print(f"  detail {short(r['name'])}: r16={r['r16_keys'][:6]} ie_later_pcs={r['ie_abs_later_pcs'][:6]} ie_later_frames={r['ie_abs_later_frames']} "
                  f"win_abs_pcs={r['A_win_abs_pcs'][:6]} bare_win_pcs={r['A_bare_win_pcs'][:6]} hbl_r16={r['hbl_any_r16']}")
    if outjson:
        for r in res:
            r.pop("irq_code", None)
        json.dump(res, open(outjson, "w"), indent=1, default=str)


if __name__ == "__main__":
    main()
