# Red-team: proposal-arm7.md ("ARM9 captures, ARM7 evaluates")

Verdict: **fixable as a research track, not as designed.** The proposal is honest about the busy class and about
what is unmeasured. But its memory plan puts DScheevos code in game memory. Its card-read latency claim is false
for the games where its hook exists. And it treats "capture" as separable from evaluation, which the core's code
contradicts. The M0-M4 measurement plan is worth keeping.

## Blockers (checked against the code)

### B1. The "free" WRAM-A range 0x037C0000-0x037DBFFF is game memory in most DS games
The proposal says (section 1.2) "No nds-bootstrap runtime component I could find uses it in DS mode". That is wrong.
- `bootloaderi/source/arm7/patch_arm7.c:405-426` (`patchVramWifiBinaryLoad`, called at :846 for every game whose
  ARM7 relocation is found) runs for every DS-only game (`unitCode == 0`) with `sdk_version >= 0x2008000`. It
  rewrites the game ARM7 autoload entry that targets 0x027E0000-0x027E01FF by adding 0x00FE0000. The game's ARM7
  segment then loads at **0x037C0000** (comment: "Relocate VRAM WiFi binary from Main RAM to DSi WRAM";
  `const u32 add = ... 0x00FE0000 // 0x037C0000`). CE7's runtime patcher does the same thing
  (`cardenginei/arm7/source/patcher/patch_arm7.c:200-268`).
- That segment's source range (0x027E0000 up to 0x027FF000) can be up to ~124 KB, which covers the whole 112 KB
  candidate. SDK >= 2.8 covers most of the commercial library.
- Also `find_arm7.c:220`: `relocDestAtSharedMem = *(u32*)0x02FFE1A0==0x080037C0 ? 0x37C0000 : 0x37F8000`. For
  headers with that MBK6 value, the game's ARM7 shared-WRAM code itself is relocated to 0x037C0000.
- Result: module7's hot code, stack and mailbox in WRAM-A would overwrite game ARM7 code or data. That breaks the
  "never touch the game's RAM" rule. M3 as written would likely miss it: its 10-game list includes cheats and the
  colour filter but no SDK >= 2.8 game in which the relocation applies, and no wireless session.

### B2. Card reads are NOT served by the FIFO IRQ when the ARM7 halt hook exists (the case A7 relies on)
Section 1.5 and section 4.3 item 7 say a request that arrives during the pass "is served by the FIFO IRQ path".
- When `patchSwiHalt` hooks the site, it sets `newSwiHaltAddr = 1 // Let ce9 know not to ping arm7 IPC`
  (bootloaderi patch_arm7.c:190).
- `hook_arm9.c:252-254` sets `b_pingIpc` only when `newSwiHaltAddr == 0`.
- CE9 then sends `IPC_SendSync(0x4)` only `if (ce9->valueBits & pingIpc)` (cardenginei/arm9/source/cardengine.c:376,
  cardDma.thumb.c:129,182).
- So in every hooked game, ARM9 card requests are served **only** by `runCardEngineCheckHalt` in the halt hook
  (cardengine.c:1799-1840), plus the 'PING' path only while `readOngoing`.
- Effect of A7's pass, which polls `sharedAddr[3]` only between `rc_client_do_frame` calls:
  - a request waits for the rest of the current frame's evaluation (~10+ ms for a 200k-instruction set at CPI 2);
  - after the pass, the hook does `swi 6` without re-checking, so the request then waits for the next ARM7 IRQ.
- Streaming games (cache mode, audio/FMV streaming) stall their ARM9 threads, which means lag. This is
  game-visible cost in the very games where the hook exists.

### B3. "Capture" is not separable from evaluation in the current core
The proposal assumes the ARM9 keeps a self-contained capture (~12-17k instructions) while rc_client and its heap move
to ARM7-owned NWRAM. The core (dscheevos/core/src/dscheevos.c) shows otherwise:
- The address table and the chain copies are compiled from rcheevos' own memref lists (`copy_chains(c, memrefs)`,
  :1185-1320, `direct_index`/`mod_index` walk `rc_memrefs_t`). With the heap in ARM7-master NWRAM slots, the ARM9
  cannot read those lists at all: a slot whose master is the ARM7 is not mapped on the ARM9 side.
- Rare chain shapes "go through rcheevos' own code" during capture (`run_step`, `rc_read_memory(...live_read...)`
  at :986). The hot rcheevos code would sit in ARM7-only memory.
- Remember/Recall chains, and sets with more than `max_addresses`, are **read live at evaluation** (`live_read`,
  :845; `live_reads`). On the ARM7, a live read:
  - sees stale main RAM (write-back D-cache, design-notes.md:22-24);
  - cannot read DTCM at all;
  - adds main-RAM traffic that `a9_waiting` doesn't gate.
  That is a silent correctness failure, which is the very thing section 3.7 rejects for the busy variant.
- The address table grows on the evaluator ("added to the table"). A7 needs a cross-CPU protocol to republish the
  capture program, and the proposal says nothing about it.

## Serious

1. **Boot ordering / slot ownership is wrong as written (section 3.3 step 2).**
   - `initMBK` (main.arm7.c:227-256) makes every WRAM-B/C slot ARM7-master (MBK2-5 bytes 0x8x/0x9x, bit0 = 1).
   - `ARM9_DSCHEEVOS_INIT` (dscheevos_hooks.c:260) runs before `ARM9_SETSCFG` (main.arm7.c:2653), which is when
     `transferToArm9` happens.
   - So at `module_init` the ARM9 cannot see B/C at all. Equal MBK7/8 mappings are not the same thing as ownership.
   - Needs an explicit WRAMONARM9 / WRAMONARM7 handshake for the chosen slots around init, plus a D-cache clean,
     ordered against the ARM7's ROM-in-RAM spill and the FNT/FAT pre-cache writes to 0x03700000
     (main.arm7.c:889-890, 1360-1366).
2. **The owner ruling isn't met.** "Frames must ALWAYS be evaluated, for EVERY game" (design-notes.md:42) is not
   satisfied by:
   - the busy class: none, even with A7-busy, Golden Sun needs f >= 0.8;
   - large sets (> ~400 KB heap): they fall back to the ARM9, which for `bios` games puts `rc_client_do_frame`
     back in the non-preemptible SWI 5 wait, the problem A7 claims to solve;
   - any game where the ARM7 site is unhooked: Thumb ARM7 without a `newSwiHaltAddr`, or `altReadMethod`
     (patch_arm7.c:160-176);
   - low-f games: the queue grows during gameplay and drops after 1024 frames.
   The proposal also says busy = "2 (~3%)". It ignores the brief's DKR finding: busy is >= 3/64 and is a floor.
3. **Bus-cost condition (b)/(c) is violated by the design itself.** Rich presence formatting, ping/award request
   building (rc_api strings, md5), newlib helpers and rc_client's scheduled callbacks are "cold code in main RAM"
   run by the ARM7 (section 3.2), ungated by `a9_waiting`. Every do_frame-reachable function must be in NWRAM, or
   the zero-contention claim fails.
4. **Single ARMv4T image regresses the ARM9 fallback path and memory.** Today the module is Thumb with ARM hot
   files (module/Makefile:18, RC_HOT_FLAGS). Two options:
   - ARM9 runs the v4T ARM-mode image for large sets: bigger code, more I-cache pressure, slower than today.
   - Two builds of rcheevos/core: more KB taken from the ROM cache.
   "Net ROM-cache effect roughly neutral" is asserted, not computed. `DSCHEEVOS_CACHE_RESERVE` is a build constant,
   and the ARM9-side capture arena, network/TLS and fallback heap stay.
5. **M4 misses NWRAM-vs-NWRAM contention.** The ARM9 reads ROM data and FNT/FAT from WRAM-B/C during play
   (CE9 cardengine.c:902-903). Whether ARM7 traffic in other B/C slots stalls those reads is untested. M4 only
   probes ARM9 main-RAM misses.
6. **The ARM7 hook site is likely a working loop, not an idle loop.** `findSwiHaltOffset` keys on a DISPSTAT
   constant in the first 4 KB of the ARM7 binary (find_arm7.c:329-360). That is a VBlank/VCOUNT-related main loop,
   not obviously the SDK idle thread. The proposal rightly gates on M0. Expect M0 to fail more often than its 60/64
   pass mark assumes.
7. **CPI model.**
   - The instruction counts are from a Thumb ARMv5TE build that includes capture (`dsc_idle + dsc_capture`).
     ARM-mode v4T counts will differ (no CLZ/BLX/LDRD; ARM is denser per instruction but has a different mix).
   - The 30/5/15/50 mix is a guess. Function-heavy rcheevos code (LDM/STM prologues, switch tables) pushes CPI up.
   - The capacity table should be stated as "pending M2" everywhere. The ≤150k "fits at any f >= 0.5" claim is
     currently presented as a result.
8. **melonDS Thumb-timing caveat applies to M1 too.** Game ARM7 code is mostly Thumb, so melonDS ARM7 idle is an
   upper bound as well. M1's ±10% hardware agreement criterion is good. Treat melonDS f only as a ceiling.
9. **Late-entry `bios` frames.** If the capture is abandoned because VBlank arrived (start-over rule), the frame is
   evaluated as a repeat. A game that habitually enters SWI 5 late gets repeats every frame, which means hit-count
   divergence from RetroArch. Needs a counter and a pass mark.

## Minor
- TWEWY is 4.6M in the table vs 4.8M in the brief.
- Notices/list written by the ARM7 must be read by the ARM9 through the uncached mirror or after invalidation.
  "Uncached stores" is meaningless on the ARM7, which has no cache.
- The display requirements (PC ranges, NDMA3 arming at wait entry, B1 discrimination) are deferred to other
  proposals. A7 only offers ARM7-side facts (PXI/card in flight), which is useful but partial.

## Fixes
1. Drop WRAM-A entirely. Put module7 code, stack and heap in ARM7-master WRAM-B/C slots chosen per session. Extend
   M3 to SDK >= 2.8 games, a wireless session, and DSi-header (MBK6 = 0x080037C0) titles, with
   `hasVramWifiBinary` logged.
2. When A7 is active, force `b_pingIpc` in CE9 (hook_arm9.c:252), so card requests take the IPC IRQ path during
   our pass. Also re-run `runCardEngineCheckHalt` after the pass, before `swi 6`. Add a p99 card-latency pass mark
   for a streaming cache-mode game.
3. Redesign the split. The ARM9 keeps a self-contained capture plan (address list plus compiled chains with no
   pointers into rcheevos' heap). The ARM7 sends plan updates through a mailbox, and the ARM9 rebuilds at a safe
   point. Remember/Recall and over-`max_addresses` reads must be captured on the ARM9 (never live on the ARM7), or
   the set falls back to the ARM9 evaluator.
4. Add the slot-ownership handshake around `dscheevosInit`, ordered after the ARM7's NWRAM writes (ROM spill,
   FNT/FAT, CLUT).
5. Link-map audit: no function reachable from `rc_client_do_frame` or the scheduled callbacks may live in main RAM.
   Otherwise those paths are gated on `a9_waiting` too.
6. Extend M4 with a case where the ARM9 streams WRAM-B/C reads while the ARM7 evaluates in other slots.
7. Report a combined coverage table against the owner ruling: busy, large-set `bios`, unhooked-ARM7 and low-f games
   as explicit uncovered rows.

## Worth keeping
- The coherence analysis (write-back D-cache, DTCM invisible to the ARM7), and the rejection of an ARM7 capture
  without a clean.
- Rejecting main-RAM-heap ARM7 evaluation on bus grounds, with M4(c) to document it.
- Moving `rc_client_do_frame` out of the non-preemptible SWI 5 wait for small/medium `bios` sets: the strongest
  argument for the angle.
- One evaluator per session, chosen by the launcher's dry-run heap size.
- M0-M4 as cheap, design-independent measurements: ARM7 idle share, NWRAM CPI, cross-CPU contention, hook-site role.
- ARM7-side signals (card/PXI/wireless in flight) for the display strip's B1 problem.
