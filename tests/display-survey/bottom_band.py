#!/usr/bin/env python3
"""Re-score the M1 survey for the bottom-edge strip (docs/display-library.md 11).

usage: bottom_band.py GAMEPLAY.TSV RUNDIR [RUNDIR...]

The survey was scored for the top-edge window. The zero-lag rule moved the
strip to the bottom: lines 158-189 (N = 32, K = 2), shown only while the game
waits for VBlank, and ended the moment any game IRQ arrives. So, per title:
- game code running in the band doesn't matter: the strip isn't shown then;
- game IRQs in the band (H-Blank/VCOUNT raster) abort the strip, so a title
  with raster IRQs there on many frames gets no strip;
- legacy DMA writes to DISPCNT A in the band (H-Blank DMA tables) don't raise
  an IRQ and would fight the mode switch: those titles fail;
- FIFO capture or DMA mode 4 anywhere: fail.
Later RUNDIRs override earlier ones for the same title (re-runs).
"""
import csv
import glob
import json
import os
import sys

BAND = range(156, 192)          # strip lines 158-189 plus the entry/exit lines around them
RASTER_LIMIT = 0.10             # raster IRQs in the band on more than this share of frames: no strip


def band_sum(lines):
    return sum(lines[l] for l in BAND if l < len(lines))


def main():
    gameplay = {}
    with open(sys.argv[1], encoding="utf-8") as f:
        for row in csv.reader(f, delimiter="\t"):
            if len(row) >= 2:
                gameplay[row[0]] = row[1]
    runs = {}
    for d in sys.argv[2:]:
        for path in glob.glob(os.path.join(d, "*.json")):
            slug = os.path.basename(path)[:-5]
            try:
                runs[slug] = json.load(open(path))
            except ValueError:
                pass

    rows = []
    for slug in sorted(runs):
        r = runs[slug]
        frames = max(r.get("live_frames") or r.get("frames") or 1, 1)
        raster = band_sum(r.get("hbl_lines", [])) + band_sum(r.get("vc_lines", []))
        # IRQ counts are per line summed over frames; frames with any raster IRQ
        # bound the share of frames the strip would be aborted on.
        raster_frames = max(r.get("hbl_frames", 0), r.get("vc_frames", 0)) if raster else 0
        dma_dispcnt = sum(band_sum(e.get("lines", [])) for e in r.get("dispcnt", [])
                          if e.get("eng") == "A" and e.get("op") == "w" and str(e.get("ctx", "")).startswith("dma"))
        dma_other = sum(band_sum(e.get("lines", [])) for e in r.get("regwrites", [])
                        if str(e.get("ctx", "")).startswith("dma"))
        fifo = r.get("capture_fifo", 0) or r.get("dma_mode4", 0) or r.get("fifo_writes", 0)
        ie_late = sum(1 for e in r.get("ie_abs_events", []) if e.get("frame", 0) > 600)
        play = gameplay.get(slug, "?")
        why = []
        if fifo:
            why.append("FIFO capture or DMA mode 4")
        if dma_dispcnt:
            why.append("DMA writes DISPCNT in band (%d)" % dma_dispcnt)
        if raster and raster_frames > RASTER_LIMIT * frames:
            why.append("raster IRQs in band on %d%% of frames" % (100 * raster_frames // frames))
        if play != "yes":
            verdict = "unknown (didn't reach play)" if play == "no" else "unknown (partial play)"
        else:
            verdict = "no: " + "; ".join(why) if why else "yes"
        notes = []
        if raster and not why:
            notes.append("occasional raster IRQ in band: strip skips those frames")
        if dma_other:
            notes.append("DMA writes other display registers in band (%d), harmless" % dma_other)
        if ie_late:
            notes.append("absolute IE writes after boot: %d (R15)" % ie_late)
        rows.append((slug.replace("_", " ").strip(), play, verdict, "; ".join(notes)))

    print("| Title | Reached play | Bottom strip | Notes |")
    print("|---|---|---|---|")
    for t, p, v, n in rows:
        print("| %s | %s | %s | %s |" % (t, p, v, n))
    known = [r for r in rows if r[1] == "yes"]
    ok = [r for r in known if r[2] == "yes"]
    print("\nBottom-edge strip: %d of %d titles that reached play are compatible (%d%%); %d titles total, %d unknown."
          % (len(ok), len(known), 100 * len(ok) // max(len(known), 1), len(rows), len(rows) - len(known)))


if __name__ == "__main__":
    main()
