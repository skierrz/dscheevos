# rnd red-team: the ARM9→ARM7 hand-over (2026-10-13)

What was reviewed: eval-speed's ARM7 lane design in `evalspeed-team/arm7-notes.md`, i.e. the
"Partitioned-evaluation patch design" (SPLIT: one rc_client on the ARM9, claimed triggers
evaluated by the ARM7) and "Interface for rnd" (WHOLE-SET: rc_client on the ARM7, the ARM9 only
captures). The patch was checked at skierrz/rcheevos a93f5a7 in the arm7eval worktree.

Method: an evidence pass (cache, MPU, NDMA/DSP, ARM7 resources; reused from rnd's data-gather
workflow), then three hostile red-teams. The red-teams covered split-mode coherency and
ordering, split-mode timing, bus and the game's ARM7, and the whole-set ring. A decider then
checked the top claims against source. Opus 5.5 medium throughout. The fourth red-team (DSP/NDMA
helpers) died on an API timeout, so the helper verdict below comes from rnd round 1.

- [verdict.md](verdict.md): the consolidated, source-checked verdict: blockers B1-B6, serious
  issues S1-S9, what is sound, the experiment plan X1-X10, the interfaces, and an evidence
  checklist for any ARM7 concept.
- [split-coherency.md](split-coherency.md), [split-timing-bus.md](split-timing-bus.md),
  [wholeset-ring.md](wholeset-ring.md): the three red-team reviews.

## Headline
The patch's split point is sound: every memory read happens in the stock memref pass, and old
values are snapshotted before frame_begin, so results are identical by construction. The
hand-over around it is not ready. Four blockers are confirmed against source:
1. **B1:** data the ARM7 must read lands in NWRAM-B/C, which belongs to the ARM9 (MBK master =
   ARM9). That covers memref lists, triggers not placed by the alloc hook, the work list and the
   old-value arrays. The ARM7 would read zeros.
2. **B2:** the ARM7 halt hook can lose the game's wake-up. It always ends in `swi 6`, even after
   an IRQ arrived during our pass, so the game's ARM7 loop can resume up to a frame late.
3. **B3:** on SDK5 titles the `0x0C000000` "uncached mirror" is cached write-back. crt0 sets
   region 3 = 0x08000000/128 MB, C=0x4A, B=0x0A (e.g. Club Penguin EPF Herbert's Revenge). So
   there is no NCNB word to poll or publish through.
4. **B4:** the ARM9 waits in result()/frame_end with no bound and no take-back, and reset,
   unload and deserialize race the ARM7.

Bus contention (B5: ARM7 main-RAM traffic slowing the game's ARM9) is unmeasured and could
kill both modes. melonDS doesn't model it; the DSi XL lab test X6 decides it.

## B3 also concerns TODAY's module (not the ARM7 route)
The module writes the offline journal "through the uncached mirror" (`DSCM_UNCACHED`,
dscheevos_module.h:300; module.c), and the ARM7 copies the journal from RAM at safe points. On
an SDK5 title whose MPU makes 0x0C000000 cached write-back, those stores can sit dirty in the
ARM9 D-cache. The ARM7 would then save a stale journal, and an unsent unlock could be lost.
Unverified whether nds-bootstrap's MPU patching changes region 3 on the DSi path (bootloaderi).
Experiment X3 (runner MPU print at gameplay frames on SDK5 titles) settles it cheaply. Routed to
orchestrator.

## When the ARM7 route has failed (fallback trigger for ARM9-only)
Owner directive: both paths are developed in parallel, and these are the conditions that would
make the ARM7 route fail. Any one of these, for a set, sends that set to the ARM9-only path.
Every item is decided by an experiment in verdict.md §4 with pass/fail fixed in advance.
- **F1 bus (X6, X10):** in the shipping configuration the game's ARM9 is more than 0.5% slower
  (timed miss loop, DSi XL) while the ARM7 evaluates from main RAM. The same applies if PCM
  capture is not bit-identical or card-read p99 is more than 1% slower. Only remedies: ARM7
  work only while the ARM9 waits, or ARM7-mastered NWRAM. If neither fits the set, it stays
  on the ARM9.
- **F2 game ARM7 latency (X5):** sliced evaluation can't keep the post-IRQ resume delay at or
  below the slice budget, or lag differs from stock.
- **F3 equivalence (X1):** the difftest is not byte-identical for the production partition and
  random partitions, including the take-back path.
- **F4 coherency (X4, X7):** the sanitizer flags any ARM7 read of an ARM9-dirty byte in module
  ranges that the glue can't fix, or B3's per-game MVA fallback doesn't hold on hardware.
- **F5 capacity and droughts (X9 and the ARM7 cost table):** the set's ARM7 share at its real
  placement exceeds 0.7× the p10 ARM7 free time, or the worst ARM7 drought exceeds what the
  ARM9 can absorb (SPLIT: take-back on the ARM9 within its idle; WHOLE-SET: ring depth).
- **F6 memory (B1, S5):** the ARM7-readable data (memref storage, claimed triggers, work
  lists, or a whole-set ring) doesn't fit in main RAM within the footprint ruling, or needs
  NWRAM that the NWRAM lane needs more.
- **F7 net cost (X8):** the hand-over costs the game more ARM9 cycles per frame than the
  evaluation it removes, e.g. header refills, cleans and the S0 capture. SPLIT must stay at or
  below +0.2% with at most 2 extra game refills a frame; S0 at or below +1%.

## DSP/NDMA helpers for the hand-over
From round 1 plus this review: none are useful now. NDMA can't gather or follow chains, only
NDMA2 might be free, and it reads past the ARM9 cache, so a clean is still needed. The DSP is
off and SCFG-locked in DS mode, can't run rcheevos, and would compete for NWRAM. NDMA stays a
candidate only for a linear stage→ring burst copy, and only after X6 says the bus can take it.
