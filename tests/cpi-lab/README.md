# cpi-lab: ARM9 timing on the DSi

Checks melonDS's ARM9 cache timing model (which our evaluation budgets and
the launcher CPU gate rest on) against the console. DSi mode, the ARM9 at
67 MHz as in a DS-mode game, IRQs off while timing, timers 0-1 at the bus
clock. Results on screen, in the emulator log (`@@` lines) and in
`sd:/dscheevos-lab/cpi.txt`.

Build (repo root, after the module's sources are in place):

    docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 tests/cpi-lab/build.sh

On the console: copy `tests/cpi-lab/out/cpi-lab.nds` to the SD card, start it
from TWiLight Menu++ in DSi mode, wait for `DONE` (a few seconds), then copy
`sd:/dscheevos-lab/cpi.txt` back.

In melonDS (with the cache model):

    emu-lock "$RUNNER" --mode dsi --rom tests/cpi-lab/out/cpi-lab.nds --bios-dir "$DSI_FILES" \
      --nand <copy of dsinand.bin> --frames 3000 --cache-model --until DONE

What it measures:
1. Cycles per access: sequential reads with the D-cache cold and warm, one
   access per 32-byte line (every access a line fill), the uncached mirror,
   stores to lines not in the cache (write buffer) and to cached lines.
2. The core and rcheevos (built with the module's flags) on the synthetic
   typical set (`gen_set.py 60 8 1 3`), fake RAM and per-frame changes as
   `tests/perf/perf.c`'s first half: `dsc_capture` and `dsc_idle` cycles per
   frame, cold (both caches emptied before each call, as the game leaves
   them) and warm. `tests/perf/run.sh --sets typical` counts the
   instructions of the same calls under qemu; cycles / instructions is the
   CPI.

Expected in melonDS (fork with the cache model, 2026-10-12): line fill
~52 cycles, cold sequential read 8.9/access, cache hit 3.4, uncached mirror
10.3, store to an uncached line 19.3, cached store 5.2; typical set:
capture ~110k cycles, evaluation ~854k cold / ~848k warm per frame (qemu:
~14.7k and ~331k instructions, so CPI ~7.5 and ~2.6).
