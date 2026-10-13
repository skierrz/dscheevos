# rnd round 1: DSi DSP, NDMA, other DSi compute (2026-10-12)

Workflow: a cost baseline, then for each lane cited fact-gathering, two proposals (ambitious and
conservative), a hostile red-team of each, and a decider. Opus 5.5 medium throughout. All
numbers come from melonDS with the cache model (optimistic) or are derived (marked D in the
reports).

This round asked whether the hardware could take work off the ARM9 while the ARM9 still runs
the evaluation. The ARM7 route (the owner's preferred direction) is a separate rnd workflow:
helpers that gather the data for an ARM7 evaluator.

| lane | verdict | why | report |
|---|---|---|---|
| R1 DSP (Teak) | not worth it | Can't run rcheevos, even with a minimal patch (16-bit char, 124 KB data space, buggy toolchain). As a snapshot copier it saves 0-5k cycles/frame on MKDS and 30-65k on Platinum (D), so no game goes from not fitting to fitting. Costs: ≥64 KB of NWRAM-B/C, a 5th upstream hook on the SCFG/MBK lock, and bus, coherency and DTCM risks melonDS can't show. | [r1-dsp.md](r1-dsp.md) |
| R2 NDMA | not worth it | No gather or chaining, can't follow pointer chains, can't reach TCM or the cache. Of the per-frame work, NDMA could only take the copying of direct words: 0-2k cycles/frame on MKDS, 0-3k on Platinum. Evaluation is ~92% of the MKDS cost. | [r2-ndma.md](r2-ndma.md) |
| R3 other | not worth it | AES, the divider, TCM/VRAM, cache lockdown, PLD, GX, camera, Atheros and the 3DS ARM11 each fail the hard rules or do nothing for us. No rcheevos public knob skips work soundly (a minimal, provably identical patch can: change-driven evaluation, see [../rnd-change-driven/](../rnd-change-driven/)). A unity build of rcheevos is a no-op at -Os and saves ≤20k on MKDS at -O2: at most an extra arm of eval-speed's rank 7 LTO A/B. | [r3-other.md](r3-other.md) |

Shared baseline (where ARM9 time goes per game): [baseline.md](baseline.md).

## What the round found that's worth acting on
1. **Kirby's capture cost (~191k/frame) is software, not hardware.** The snapshot is re-copied
   on each of the 20-37 idle wakes a frame. That belongs to the capture lane. Don't add a
   destination "read-touch": the D-cache is read-allocate, so it only adds line fills.
2. **MKDS D-misses come from streaming the conditions (~150 KB/frame), not from memrefs (~2 KB).**
   Every condition line is also dirtied every frame (is_true, current_hits, changed). So the
   NWRAM set-data placement should put condition arrays first.
3. **NDMA0 is shared by ce9 (card DMA) and the display FIFO feeder.** Only display's "stand down
   during card reads" rule separates them. That needs an explicit ruling (display session).
4. **Read-only SCFG/MBK boot probe (proposed, not built).** Log SCFG_CLK/RST/EXT9 and MBK1-8 once
   at boot from ARM9_DSCHEEVOS_INIT. It's free in play, and it answers questions that display
   (EXT9 bit 8) and any SCFG-touching work need. It needs approval before it touches the module.
5. **Hardware tests for display's H6/H11 ROM:** the CP15 clean-by-VA cost per line (clean vs dirty
   lines), and ARM7 main-RAM latency plus 16-channel sound under ARM9 NDMA at physical blocks
   2^0/2^3/2^7. The ARM7 route needs both numbers.
6. **Corrections:** memplan's "short-circuiting active" label is wrong for this rcheevos pin.
   The rich-presence ping string is built from live (coherent, possibly torn) memory, not the
   snapshot. That's cosmetic.
