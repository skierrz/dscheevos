# Design brief: how DScheevos gets CPU time in every game (idle coverage)

DScheevos runs RetroAchievements (rcheevos, unmodified) inside retail DS games booted natively on a DSi by a fork
of nds-bootstrap. The module (dscheevos/module, ARM9, 2 MB at 0x02D80000 in the DSi's extra RAM) copies watched
game memory and evaluates achievements. Today it gets CPU time ONLY when the NitroSDK idle thread calls OS_Halt:
nds-bootstrap patches OS_Halt (`mov r0,#0; mcr p15,0,r0,c7,c0,4; bx lr`) to jump to the module's halt entry.

## Owner's goal and hard rules (non-negotiable; a breach is a blocker)
- "Find a system that gives DScheevos CPU time in games that never sleep through OS_Halt, and ideally one that
  works better across ALL games... no band-aid fixes". No per-game patches/lists as the mechanism.
- Zero gameplay cost: no added lag frames, no visual glitches, never touch the game's RAM, game Wi-Fi as stock.
  Code patches applied at boot by nds-bootstrap (like the OS_Halt patch) are the accepted precedent; writing game
  data/stacks/heap is not.
- Never use game TIMER registers (timing via REG_VCOUNT only). No SD writes during play. rcheevos unmodified,
  full feature set. Smallest memory footprint (every KB comes from nds-bootstrap's ROM cache).
- Never write NAND / firmware flash / Wi-Fi EEPROM.

## Measured facts (read these files; cite them)
- Census of the owner's 64 ROMs: repo worktree ~/Developer/DScheevos/dscheevos-idlecov, docs/idle-census.md.
  53 idle in SDK OS_Halt, 9 wait in BIOS SWI 5 (VBlankIntrWait) on the main thread (ALL NitroSDK 2.x games),
  2 never halt (Golden Sun: Dark Dawn = Camelot task scheduler spin; Code Lyoko = `flag=0; while(!flag) poll(obj);`
  busy VBlank wait calling a service routine). Estimate for RA's ~580 DS sets: ~9% BIOS, ~3% busy engines.
- All 64 ROMs have the SDK wrappers SVC_WaitIntr / SVC_WaitVBlankIntr as Thumb `movs r2,#0; svc 4|5; bx lr`
  (6 bytes) at a random offset inside the ARM9 secure area (first 2 KB of the ARM9 binary, random fill around).
  The module is out of Thumb BL range (+-4 MB) from there; a jump needs 8 bytes (ldr/bx/literal) or a trampoline in
  the secure-area padding.
- Working notes with more detail (patch sites, SWI semantics, lag risk, golden sun disassembly):
  (session notes, not in the repo)
- Hardware facts with GBATEK citations (SWI 4/5/6 on ARM9, vectors at FFFF0000, only IRQ vector redirectable
  via DTCM+3FFC, CP15 low vectors, EXMEMCNT bus priority, WRAM/NWRAM, cache, SCFG_CLK 133 MHz, DMA modes):
  (GBATEK fact sheet, session scratchpad; not in the repo)
- NitroSDK main RAM is WRITE-BACK D-cached (PU region 1 0x02000000 8 MB, C=1 B=1; verified in KSSU crt0). The
  ARM9 D-cache is 4 KB, I-cache 8 KB. So any reader other than the ARM9 (ARM7, DMA) can see stale values of game
  variables whose lines are dirty. The module snapshots on the ARM9 for this reason.
- Evaluation cost: tests/perf/realsets.sh instruction counts (memplan cpu.md: most real sets 60k-500k instr/frame,
  Digimon 1.1M, HG/SS 2.1M, TWEWY 4.8M); eval-speed measured real CPI ~3.4 (cache-miss bound) at 67 MHz, so
  e.g. 300k instr ~ 1.0M cycles ~ 90% of a 1.12M-cycle frame. The ARM7 is an ARM7TDMI at 33 MHz, no cache.
  Idle budgets per game (gameplay): (idle-budget report, idle.md; not in the repo)
- Kirby's idle thread wakes ~20x per frame; each module idle pass had ~390k cycles of fixed overhead
  (eval-speed is fixing that in dscheevos/module/src/module.c).

## Code to read
- dscheevos/module/src/start.s (halt_entry: idle-loop check, module stack, in_halt, wfi), module.c (idle pass),
  docs/architecture.md (snapshot-at-halt design, seqlock copy, VBlank commit, queue), docs/handoff.md.
- external/nds-bootstrap/retail/bootloaderi/source/arm7/dscheevos_hooks.c (patchOsHalt), dscheevos_hooks.h.
- The display session's requirements on the wait path: ~/Developer/DScheevos/dscheevos-display/docs/display-library.md
  section 11 (11.4) and docs/display-research/red-team-zero-lag.md (branch `display`). Summary: the notice strip
  runs only while the game is in its frame-done VBlank wait; it needs (1) the PC range(s) that count as "in the
  wait" per wait kind, (2) a few arming instructions at wait entry (one NDMA3 H-Blank counter), (3) strip-only IRQ
  wakes must go straight back to the wait without an idle pass, (4) ideally tell "main thread waiting for VBlank"
  from "threads blocked on ARM7 PXI replies" (OS_Halt alone can't).

## What the design must cover
1. BIOS waits (SWI 5 / SWI 4): a generic hook, exact BIOS semantics, and the lag risk (SWI 5 discards the VBlank
   flag and waits for a NEW VBlank: work that overruns VBlank then calling SWI 5 costs a whole frame). Work on the
   main thread is NOT preemptible by the SDK scheduler (unlike the idle thread).
2. Busy engines (no halt at all): is there any generic, zero-cost way? (hidden idle in poll loops; NDMA-based
   periodic IRQs that DS-mode games never use; 133 MHz; ARM7.)
3. The ARM7 option: evaluation on the ARM7 (33 MHz, memory: ARM7 WRAM 64 KB, shared WRAM, DSi NWRAM; main-RAM
   bus contention with the ARM9 = possible game cost; stale reads due to write-back cache; game's ARM7 sound/
   Wi-Fi/touch duties; ARM7 idle time is unmeasured).
4. "Better across all games": a uniform mechanism, if one exists, and the "game is waiting now" signal for the
   display strip.
5. How each claim would be measured in melonDS (the runner has a cache model and the census counters) and on
   hardware (DSi XL), with pass/fail decided in advance.

## Caveat on all melonDS idle figures
Stock melonDS charges 0 cycles for the second Thumb instruction of a fetched word (games are mostly Thumb), so
games run too fast and every idle/halted figure (census, idle.md) is an UPPER BOUND. eval-speed is re-measuring with
a corrected `--thumb-cycle` runner option. Wait-method classification is unaffected. Designs must not rely on
large idle margins; budgets must be measured at runtime on the real machine, not assumed.

## Late finding (after the census doc was first written)
Diddy Kong Racing DS (SDK 3.x) never halts in its 3D engine: a game THREAD spins `for(;;) consume(obj)` on a ring
buffer's read/write indices (0x0201A344 / 0x0201A294), so the SDK idle thread never runs (0% halted for 4000+
intervals of in-engine scenes). So busy engines include spinning game threads on top of the SDK scheduler, and
menu-only census runs can miss them: the busy share of the library is a floor (>= 3 of 64 = ~5%).

## Late facts (2026-10-12 23:00) for the judge
- Owner rulings: frames must ALWAYS be evaluated, for EVERY game (no exclusions, no refusing, no softcore fallback).
  All budgets at 67 MHz; 133 MHz is a test-only flag (not a design lever). Our compute on the game's CPU as close
  to zero as physically possible.
- Cache pollution: the module's ARM9 idle work costs the game ~100k cycles per frame in cache refills afterwards
  (Kirby: +21% game cycles; eval-speed). Any ARM9-side mechanism that runs more of our work pays this too; favour
  small ARM9 cache footprint (NWRAM, ARM7).
- ARM7 measured (census runner, --thumb-cycle, achievements off, Wi-Fi off): in gameplay the game's ARM7 is halted
  83-92% (p10) of each interval, ~465-515k ARM7 cycles free per interval, in 8 games (Kirby SSU, MKDS, Sonic Rush,
  Platinum, Black, Digimon, TWEWY, SoulSilver). Boot/loads: worst 1024-interval window down to 44-80%.
- ARM9 D-cache dirty lines among a set's direct watched addresses at VBlank: Kirby 9% of intervals, MKDS 1%,
  Sonic Rush 61%, Platinum 88% (mean 1.8 of 34 lines). ARM7 must not read game RAM directly.
- rnd session owns DSP (Teak), NDMA, hw divider/sqrt, AES ideas; eval-speed's arm7eval agent owns the ARM7 build
  of rcheevos and its throughput. Shared notes: ~/Developer/DScheevos/evalspeed-team/arm7-notes.md.
