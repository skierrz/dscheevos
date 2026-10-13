# Design round 2 brief: busy engines, the universal frame-boundary capture, and ARM7 scheduling

Read first: the round-1 brief (~/Developer/DScheevos/dscheevos-idlecov/docs/idle-coverage/brief.md) and the round-1
decision (docs/idle-coverage/decision.md, same worktree), plus its proposals and red-teams in that directory.
Round 1 settled the BIOS-wait gate (P1a), the unified coroutine W parked by a DTCM+3FFC swap (P1b), and ARM7
evaluation as phase 2. It concluded "no generic zero-cost mechanism exists for busy engines" and asked the owner (Q4).

## Why round 2
The owner has since ruled, explicitly and repeatedly: **frames must ALWAYS be evaluated, for EVERY game, no
exclusions, no refusing, no softcore fallback**, and our compute on the game's CPU must be as close to zero as
physically possible. All budgets at 67 MHz (133 MHz is a test-only flag). Detect-and-tell is not acceptable as the
end state. Round 2 must find the best mechanism for games that never wait (busy engines), and settle where/when the
ARM7 evaluator runs, with evidence. If something truly cannot be done at zero cost, quantify the minimum cost
exactly and say what test would prove it causes no lag frames.

## Facts added since round 1 (cite these files)
- Busy engines in the owner's 64 (docs/idle-census.md): Golden Sun: Dark Dawn (Camelot task scheduler spin, loop
  0x020371A4-0x020373B8, VCOUNT/time-based), Code Lyoko (`flag=0; while(!flag) poll(obj);` at 0x020571CC, poll =
  0x0208D3E0 a service routine; flag set by an IRQ), Diddy Kong Racing DS 3D engine (an SDK game THREAD spins
  `for(;;) consume(obj)` on a ring buffer's read/write indices, 0x0201A344/0x0201A294, so the SDK idle thread never
  runs). The menu-only census can miss more: busy share >= 3/64 is a floor. ROMs: ~/RomM/library/roms/nds (owner's
  dumps; read-only; never copy into the repo). You may disassemble with the devkitARM docker image
  (devkitpro/devkitarm:20241104, arm-none-eabi-objdump -D -b binary -marm [-Mforce-thumb] --adjust-vma=...) after
  extracting the ARM9 binary with ~/Developer/DScheevos/dscheevos-idlecov/tests/perf/waitscan.py (arm9_static(),
  overlays()). Docker builds go through ~/Developer/DScheevos/bin/build-lock.
- ARM7 free time measured (census runner, --thumb-cycle, achievements off, Wi-Fi off), gameplay: 83-92% (p10) of
  each interval halted, ~465-515k ARM7 cycles free per interval, 8 games; boot/loads drop the worst 1024-window to
  44-80%. Table: ~/Developer/DScheevos/evalspeed-team/arm7-notes.md ("Answers from idle-coverage").
- ARM9 D-cache dirty lines among a set's direct watched addresses at VBlank: Kirby 9% of intervals, MKDS 1%, Sonic
  Rush 61%, Platinum 88%. So any reader other than the ARM9 needs an ARM9-side copy or clean.
- Capture cost per frame (instructions, qemu, dsc_capture: the module's copy of the watched data): most real sets
  0.5k-5k instr (Kirby 873, MKDS 1,540, Sonic Rush 1,084, Phoenix 844), Platinum 23.5k (pointer chains), Digimon
  21.6k, HG/SS 121.7k (address table overflow). Table: docs/idle-census.md does not have it; it is in the round-1
  brief's cpu.md reference (memplan cpu.md) and tests/perf/realsets.sh output. Real CPI ~3.4 for evaluation;
  capture is a memory-bound copy (CPI likely higher per access on misses).
- Our ARM9 idle work costs the game ~100k cycles per frame in cache refills afterwards (Kirby +21%).
- RetroArch/emulator reference semantics: rcheevos is evaluated once per emulated video frame, on memory as it is at
  the frame boundary (VBlank), including lag frames where the game is mid-logic.
- rcheevos may now be modified ONLY where evaluation results are proven identical to stock (differential tests on
  every real set with real RAM dumps), minimal, offered upstream (CLAUDE.md, owner ruling).
- Other sessions: rnd is designing the DATA-GATHER piece (a coherent per-frame memref copy for an ARM7 evaluator at
  ~zero ARM9 cost: D-cache clean by MVA + NDMA gather, Teak DSP gather, uncached-destination ARM9 copy) and owns
  NDMA/DSP/AES/divider ideas; do not redesign those, but you may assume their interface "a coherent copy of the
  plan's words exists in NWRAM at time T, cost X" and state what T and X you need. eval-speed's arm7eval agent owns
  rcheevos built for the ARM7 and its throughput (CPI) -> assume ~CPI 2-3 from NWRAM, unmeasured.
- Game-cost evidence rule: any runtime change must report the game's own ARM9 cycles per frame off vs on (Kirby,
  MKDS, one busy game). Machine-load rules: 2 emulator slots shared by all sessions; do not run the emulator in this
  design round (static analysis, code reading and arithmetic only).

## Hard rules (unchanged; a breach is a blocker)
Zero gameplay cost (no added lag frames, no glitches, never write game RAM; boot-time code patches by nds-bootstrap
are the accepted precedent), no game TIMER registers, no SD writes in play, never write NAND/firmware/Wi-Fi EEPROM,
smallest memory footprint, hardcore compliance.
