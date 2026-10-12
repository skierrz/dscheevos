# Red team: docs/display-library.md (display library design, 2026-10-12)

Read-only review. Nothing was run (no melonDS, no tests). Sources:
- DL = `dscheevos-display/docs/display-library.md`
- HW = `docs/display-research/hardware.md`
- OR = `docs/overlay-research.md`
- NB = `dscheevos/external/nds-bootstrap/retail/`
- G = `(local scratch)`
- MEL = `dscheevos/tools/melonDS/src`

Anything marked **(spec.)** is my inference and has not been verified.

Memory-plan facts from the coordinator, used below:
- At most 16 KB resident, plus a window of at most 32 KB that exists only while something is on screen.
- No hardcoded addresses.
- NWRAM-B/C belongs to the ARM7 and holds FNT/FAT.
- 0x02FE0000+ and 0x02FDC000-0x02FFC000 are taken by some games.
- About 0 idle CPU when nothing is shown.
- Strings stay in rc_client and are read through its API.

---

## Blockers

### B1. A game read-modify-write of DISPCNT that spans our line-N-1 IRQ leaves mode 3 on for the rest of the frame
DL R1 (line 198) says "A game read-modify-write keeps our 3 and we put back 1". That only holds if the game's RMW *finishes before* our IRQ.

NitroSDK setters are non-atomic LDR/BIC/ORR/STR sequences with IRQs enabled, for example `GX_SetVisiblePlane` and `GX_SetGraphicsMode` (spec. on exact codegen). Suppose such a sequence has already loaded DISPCNT (mode 3) when the NDMA2 IRQ at line N-1 runs and writes 1. The game then stores 3. Mode 3 stays on and nothing corrects it until DL step 4, "next VBlank" (DL:180-181). After line N+M the FIFO is empty, so lines N+M..191 show underrun garbage over the game for a whole frame. GBATEK and OR don't say what underrun looks like (OR:180).

The same race at line 261 is harmless: the strip is just missed that frame.

**Fix:**
- Add a fourth event: an NDMA IRQ at line N+M-1 that re-checks mode and puts back 1. NDMA0 is still feeding the FIFO until then, so a game's stale write that lands in lines N..N+M-1 only thickens our border.
- In M1, log every DISPCNT A *read* during lines 261..N+M with its PC, not just writes (HW:323 logs writes only). This finds both the RMW sequences and the save/restore patterns in B5.

### B2. IRQ delivery latency decides correctness, and the design has no abort rule and no measured bound
- **Entry.** The mode-3 STRB must land between line 261 and the start of line 0. That is about 2 lines, about 127 µs, about 8.5k ARM9 cycles. Things that can delay it:
  - a game IRQ handler running then (NitroSDK doesn't nest);
  - an IRQs-off section;
  - a legacy DMA burst. G:12769-12774: "interrupts executed during DMA will usually halt the CPU (unless the handler uses only TCM... and may not access IE, IF or other I/O)". `OS_IrqHandler` reads IE/IF.
  - Games run VRAM uploads during VBlank, and on heavy frames those spill toward line 262.

  DL step 2 says only "re-check the guards" (DL:177). It needs an explicit rule: if VCOUNT ∉ {260,261,262} with IRQs off at the moment of the STRB, don't enter.
- **Stale IRQs.** The same rule must cover them. If the game clears IE bit 30 while NDMA2 completes, IF.30 stays latched. It fires later, at a random line, when IE is set again: by our OR-back, or by the game restoring a saved mask.
- **Exit.** A switch-back that is L lines late shows max(0, L-M) lines of underrun on game lines. M is not specified anywhere. Nothing measures ARM9 IRQ latency in lines 0..N+M on real games.
- **FIFO starvation by CE9 is not a hypothetical.** CE9 card copies use `0x90070000`: start immediately, physical block = 2^7 = 128 words (`common/include/ndma.h`, `cardDma.thumb.c:230,350,442,462`, `cardengine.c:587`). G:27919-27922: "a single (physical) block transfer cycle will never be split up". A 128-word main→main copy is all non-sequential (G "NDS Sequential Main Memory DMA") and takes about 1 line of bus. The display FIFO holds 8 words (MEL `GPU.cpp` DispFIFO[16] halfwords).
  - Moving CE9 to NDMA1 doesn't help. Priority only applies *between* blocks.
  - H6's pass mark ("no banner errors under a 128-word NDMA1 copy") will very probably fail.
  - The priority of legacy DMA against NDMA is unknown (G:27880-27881).

**Fix:**
- Fixed VCOUNT windows at both handlers.
- Choose M from a measured latency distribution. New test: on every shown frame, log the VCOUNT and HBlank phase at handler entry, in the 4 M7 games. Pass mark: 0 frames late beyond M in 30 min.
- For card reads, either:
  - drop CE9's physical block size to 4-8 words, which costs load time and so is game-visible, so the owner must rule; or
  - stand the strip down while CE9 has a read in flight (`dmaDirectRead` / ARM7 `readOngoing`) and during legacy immediate DMA. Some DMAxCNT reads are possible, but a game DMA can start mid-strip, so this is spec. coverage only.

### B3. IE bits 28/30: an unpatched dispatcher means a crash or an IRQ storm, the game can see the bits, and a lost IRQ means B1
- **Crash or storm.** NitroSDK `OS_IrqHandler` indexes `OS_IRQTable[31-clz]`, which has 22 entries (OR). Two cases:
  - a signature miss (another SDK variant, an inlined copy, a handler in an overlay);
  - the game or middleware installing its own vector at DTCM+3FFCh (G:12873-12878).

  In either case, IE.30 with IF.30 set jumps through garbage: a crash. A custom handler that loops `while (IE&IF)` and never acks bit 30 gives an IRQ storm, which is a hang.

  **Fix:** set IE.28-31 only after checking, on every arm, that [DTCM+3FFC] == the patched handler's address.
- **Missed residual: the game can read IE.** With SCFG_EXT9.8 set, `OS_GetIrqMask` and the return value of `OS_SetIrqMask`/`OS_DisableIrqMask` include our bits. Games save and restore those masks. Not in R1-R14. Add it as a residual.
- **Absolute IE writes.** nds-bootstrap protects IPC_SYNC only *lazily*. `enableIPC_SYNC` runs from card-read paths (NB `cardenginei/arm9/source/misc.c:128-134`). It isn't continuous. If the same scheme is used for bits 28/30, an absolute IE write at lines 0..N-1 loses the switch-back IRQ, and B1 follows. B1's N+M-1 verify IRQ doesn't help: it uses the same IE bit.

  **Fix:** M1 already logs absolute IE writes (HW:326). Make "any absolute IE write that clears bit 30 during lines 261..N+M" an exclusion criterion per title.

---

## Major

### M-a. The "next VBlank" fixer clobbers a game's legitimate mode 3
DL:181 reads "if mode 3 is set and we didn't set it, restore 1". As written, it rewrites the game's register when *the game* chose mode 3, for example an FMV through the main-memory FIFO (the guard only checks mode == 1 at entry).

**Fix:** put back 1 only when:
- we set it this frame and it is still ours; or
- a leak is proven (`GXi_DispMode` == 3 and no legacy DMA in mode 4).

### M-b. The strip will flicker: arming depends on the IPC IRQ, which is missing on 0-15% of load frames
- OR:119 measured that 0-15% of frames got no IPC IRQ during card loads.
- The ARM7 sends 0xA from its VBlank handler, after the cheat engine runs (NB `cardenginei/arm7/source/cardengine.c:1982-1983, 2038-2061`).
- The ARM9 hook runs on whichever IPC comes first after the count changes (`cardenginei/arm9/source/cardengine.c:1641-1646`).

A missed arm drops the strip for one frame in the middle of a notice. That is visible flicker. DL says nothing about it.

There is a second timing problem. VCOUNT at the hook gives the line but not the dot phase, so a one-shot computed from it lands within about ±1 line. That is not enough margin for the 2-line entry window in B2.

**Fix (simpler, see S1):** use a hardware-phased timer. Two options:
- NDMA startup 06h (V-Blank) with a BCNT interval. This is phase-locked to line 192, needs no IPC dependence, and needs no work in the game's IPC IRQ.
- Self-re-arm from the line-N-1 IRQ, which has a known HBlank phase, with prescaler 2.

The prescaler-1 / 123-line limit in DL is self-imposed. Prescaler 2 gives 0.48 µs resolution and a 31 ms range (G:27925-27931).

### M-c. Start mode 07h doesn't count V-Blank lines, so TCNT is off by one
G:27965: "07h H-Blank (but not during V-blank)". Armed at line 261, the first count is the HBlank of line 0. TCNT=N-1 then fires at the HBlank of line N-2, not N-1 (DL:179, HW:84). This is cosmetic (the strip loses a line), but it changes the timing everyone has been reasoning about.

**Fix:** TCNT=N. H2 should test 07h counting too.

### M-d. R8 understates the IRQ residual: our line-N-1 handler collides with game raster IRQs at line N
Top HUD splits at lines 16, 24 or 32 (VCOUNT IRQ) and per-line HBlank IRQ effects are common (spec. on prevalence). N is 16 or 32.
- Our handler runs in the HBlank of N-1. That HBlank is about 99 dots, about 1.6k ARM9 cycles. Our handler costs about 750-900 cycles plus dispatch (OR:119).
- So it can push the game's line-N register write into line N's display: a torn first game line under the strip.
- On the game side this is a visible glitch, not "a few µs".

**Fix:**
- DISPSTAT is readable, so read LYC.
- Choose N so that LYC ∉ [N-2, N+2], and stand down if DISPSTAT.4 (HBlank IRQ) is enabled.
- Add to M1: game IRQ entries within ±2 lines of N.

### M-e. Test pass marks hide exactly these failures
- M7 "0 pixel differences below line N+M" (DL:261) should be **below line N**. Lines N..N+M-1 are the game's. Count the frames where the margin showed, as a separate metric.
- H7 "lag equal to stock" on hardware isn't reproducible with live input. Use deterministic attract demos (Kirby and others), or a statistical pass mark: N runs, a bound on the lag difference.
- H6 needs CE9's real 128-word blocks (B2).
- H2 should also:
  - measure whether a channel waiting out its BCNT interval blocks lower channels or legacy DMA (spec.: a fixed-priority arbiter might hold the grant);
  - test 06h and 07h with intervals.

### M-f. The idle-slack gate is a probability reducer, not a "no added lag frame" guarantee
- Achievement triggers coincide with load spikes: boss kills, stage clears, transitions. The notice is shown on exactly the frames after the event. An 8-frame look-back taken in calm play doesn't predict them.
- Any frame whose own load falls within `cost` of the deadline turns into a lag frame. That holds whatever the history was.
- The module's own evaluation already eats idle time. The handoff notes about a frame of CPU per frame for the synthetic set (`docs/handoff.md` item 4). The gate must use slack *after* module work.
- The ARM7 isn't gated at all: main-RAM contention against sound, Wi-Fi and SD.
- During card loads the ARM9 waits in `sleepMs`, which counts as "idle", so the gate opens exactly when CE9 copies need the bus.
- 30 fps games: per-VBlank idle alternates. Measure per game-frame.

**Fix:**
- State the residual honestly: "P(added lag frame) per shown frame", measured in M7.
- Require slack ≥ cost + the maximum frame-to-frame load increase over the last 64 frames.
- Delay showing by K frames after the triggering event.
- Stand down during card I/O.

Bus cost arithmetic: DL/HW assume 42% occupancy across the whole line. FIFO requests only occur every 8 px during the 256 visible dots (MEL `GPU.cpp:1117-1121`), so the real figure is about 640/2130 = 30% per strip line, or about 3.2% of the frame for 28 lines. That is minor and conservative. Main RAM is the real case (no NWRAM, per the memory plan, and G:8193 says "source must be in Main Memory").

### M-g. Memory claims don't add up against the 16 KB plan
**Missing from the ~12-14 KB:**
- presenter code (NDMA, guards, IRQ handlers);
- the `OS_IrqHandler` and `GX_DispOff` trampolines;
- the pack index (about 1 KB per 100 achievements if resident, otherwise an SD read per notice);
- the Japanese glyph extension (unbounded; 8x8 kanji are barely legible, 12x12 is 18 B per glyph);
- the queue's string copies.

**Strings.** `dsd_item_t` copies line1/line2 (96 B each, DL:67). The module ABI already copies strings:
- `dscm_notice_t` is 116 B × 16 (about 1.9 KB);
- `dscm_list_entry_t` is 176 B per achievement: about 17.6 KB for 100 and 53 KB for 300 (`module/include/dscheevos_module.h:219-235, 259-282`).

The memory-plan rule "strings stay in rc_client" contradicts both of these, and `dsd_render_list` depends on `dscm_list_t`. `arm9_igm` can't call rc_client directly. It would need a function table exported by the module and called while paused.

**The "window only while visible".** It is either a permanent reservation, or it needs a CE9 cache-eviction protocol: evict cache slots, never while a read is in flight, and refill afterwards. A refill means longer loads, which the game can see. Neither appears in DL §5.

**Single buffer.** "Single-buffered; swap only at VBlank" (DL:141) contradicts itself. A re-render while the strip is up (tracker value, "+N more") tears our strip.

**Fix:** pick one:
- 2×16 KB within the 32 KB window;
- only write the buffer between line N+M and line 260;
- re-render only while hidden.

**Cache coherence.** The FIFO source must be flushed (DC_FlushRange) or written through the uncached mirror (`DSCM_UNCACHED`). DL doesn't say which.

### M-h. The SD asset path doesn't exist yet, and adding one perturbs the code that just produced the Kirby race
There is no module-callable read today. It would need:
- a cluster map for `<gameid>.pak` resolved at boot, as for `pending.bin` (`cardenginei/arm7/source/cardengine.c:2003-2024`, `getFileFromCluster`). A FAT walk at runtime means extra FAT reads.
- a new ARM7 command, serialized with CE9 `sharedAddr` reads, `readOngoing` (:1416-1492) and the SDIO shim.

Every ARM7 SD read:
- lengthens the game's next ROM read, so streaming games (Kirby's attract) can stutter;
- moves ARM7 timing. That is exactly how the CMD_BUSY race surfaced (commit d3dc0b3: a 120 µs Timer1 IRQ in a two-instruction window).

**Fix:**
- No SD reads for the strip during play (S2/S3).
- Or, if reads are kept: one non-blocking read per notice, only when CE9 has been idle for K frames, soaked with `--sd-cmd-busy 0` plus network on.

### M-i. The GX_DispOff patch (R3) is narrower than the leak
- **Inlined and overlay copies (spec.).** `GX_DispOff` is a few instructions. Some SDK versions and games may inline it, or carry it in overlays. Boot-time patching covers only the main binary (the `OS_Halt` patch class). D3 must count *every* copy, including in overlays.
- **The leak is wider than GX_DispOff.** Any code that saves DISPCNT wholesale during lines 261..N-1 and restores it later reproduces R2 with a whole frame of underrun: custom engines, scene fades, Mobiclip (spec.).
- The read log from B1 finds these.

### M-j. R4 mis-describes upstream precedent
nds-bootstrap's IPC hook is not a code patch. It writes the game's `OS_IRQTable[16]` in DTCM at runtime (`misc.c:117-121`, `hookIPC_SYNC`). The same goes for VCOUNT `[2]`, and `cardengine.c:1670-1675` re-asserts it every IPC.

So "never touch game RAM" is already bent upstream. The owner should know this when ruling on R4.

It also opens an alternative to per-SDK signature patching (S4): wrap the BIOS vector at DTCM+3FFC. Handle IF.28-31, otherwise tail-call the game's handler. One mechanism works for every SDK, at the cost of one DTCM word, which games rewrite at init. Re-assert it the way `hookIPC_SYNC` does.

### M-k. P4 compares and restores per word, so it can leave our layer bit on for good
Suppose the game RMWs DISPCNT_B, changing other bits, during a notice. The word no longer equals "ours", so we never restore it, and our layer-enable bit stays set. It points at a BGCNT and bank we then restore or release: the game shows a garbage layer from then on.

**Fix:** compare and restore per *bit or field* we own.

---

## Minor
- **SCFG_EXT9.8 (R5)** also makes IE/IF bits 14, 15 and 24-27 settable (G:12829-12844). An NTR game that sets broad masks (rare, spec.) could take IRQs it never had. Set the bit in every NTR boot and reset path, not just the first boot (`bootloaderi/source/arm9/main.arm9.c:429-436`).
- **NDMA ownership on the ARM9 side.** CE9 uses NDMA0 and alternates 0/1 (`cardDma.thumb.c:88-90, 461-462`), plus NDMA0 in `cardengine.c:587`. Nothing on the ARM9 side uses NDMA2 or NDMA3. The ARM7 has its own NDMA bank: SD uses `sdmmc_set_ndma_slot` (`cardenginei/arm7/source/cardengine.c:337, 1320-1384`) and clears all four channels at reset (:669-671). That is ARM7-only, so no conflict.
  - After R6 (moving CE9 card reads to NDMA1 only), `endCardReadDma`'s `ndmaBusy(0)||ndmaBusy(1)` test (`cardDma.thumb.c:88`) would see the FIFO channel busy on strip frames and delay card-read completion. Change that test as well.
- **Late arming.** "Transfer starts at next frame" (G:8195). In melonDS, `RunFIFO` is latched at `StartFrame` (`GPU.cpp:1135`) and mode-4 DMAs are stopped at line 192 (`GPU.cpp:1297`). That is consistent with arming at 261. melonDS also runs FIFO requests in mode 1, so leftover margin drains, but hardware is unknown. Always write NDMA0CNT=0 and confirm busy=0 before re-arming. G:28016-28019: while a channel is enabled, maybe only the busy bit is writable.
- **IRQ stack.** The NDMA handlers must switch to the module's IRQ stack, as `vblank_entry` does (`module/src/start.s:128-138`). If they didn't, they would push onto the game's IRQ stack in DTCM.
- **Halt wake-ups.** Each NDMA IRQ wakes `OS_Halt`, which re-enters `halt_entry` (start.s:163-206): 3 extra idle passes per frame and distorted idle statistics. Coordinate with the perf session.
- **Polling cost.** The guard history (30-frame stability, 8-frame slack) needs polling every frame even when nothing is shown. The plan says ~0 idle CPU. Start polling only when a notice is queued, and accept about 0.5 s of latency.
- **Scheduling: leaderboard order.** "Leaderboard started" (priority 4) is queued separately from failed/submitted (priority 3). A late "started" can show after a "failed". Use one slot per leaderboard id for all LB events, and drop stale ones.
- **RA compliance (spec., needs RA's answer).** With an allow list plus a gate, many titles will only ever have LED cues. Ask RA whether hardcore approval needs on-screen unlock pop-ups and visible trackers and challenge indicators. A tracker is only pinned by owner option (DL:101), so LB values are otherwise invisible during a run. The boot placard covers the hardcore indicator; keep it.
- **3DS.**
  - The camera-LED claim is untested on hardware. The existing LED path returns early for `consoleModel >= 2` (`cardenginei/arm7/source/cardengine.c:1988`).
  - On 3DS, P5 (TwlBg) makes P2 unnecessary. Don't enable P2 there by default.
- **Badges on SD.** About 520 B per badge is fine. If all badges move into a pre-rendered pack (S2), size per achievement is about 4 KB, still small on SD.

---

## Simpler alternatives the design overlooked
- **S1. A VBlank-triggered NDMA (06h) with a BCNT interval as the line-261 timer.**
  - It is phase-locked to line 192 in hardware and needs no IPC dependence.
  - It puts no code in the game's IPC IRQ and fixes M-b.
  - In repeat-infinitely mode it gives one IRQ per frame at 192+delay. Arm it only while a notice is up.
  - Needs H2 to confirm interval semantics in event-start modes.
- **S2. The launcher pre-renders each achievement's notice** (title, description and badge) as a 4bpp or 1bpp bitmap in the pack.
  - The DS side only expands it to RGB555 and draws dynamic numbers with the ASCII font.
  - That removes the proportional font, UTF-8 and Japanese glyph handling, and most of the 6-8 KB renderer.
  - It cuts resident memory and makes golden tests trivial.
- **S3. A text-only strip,** with a generic trophy glyph and badges only on the menu Now page (paused). That means no SD reads during play.
- **S4. A BIOS-vector wrapper** in place of per-SDK `OS_IrqHandler` signatures (M-j).
- **S5. A verify IRQ at N+M-1** (B1). It is cheap and turns B1 and late-exit failures into a thicker border.
- **S6. A CPU-free switch-back (considered, not recommended).** NDMA could write a DISPCNT snapshot taken at line 261 with mode=1. That removes the latency-critical exit IRQ, but it clobbers any game DISPCNT write in lines 0..N-1. That is worse than B1 unless M1 shows no title writes DISPCNT in that window.

---

## What I would change in the build order
1. Before any owner ruling, add to M1:
   - DISPCNT reads;
   - game IRQ entry lines near N and 261;
   - absolute IE writes;
   - legacy DMA bursts spanning lines 255-262 and 0..N+M.
2. Fix the M7 pass mark (below N, not N+M).
3. Add H2b (interval blocking, 06h/07h counting) and H6 with CE9's real block size.
4. Add a latency-distribution test on real games (B2).
5. Decide S2/S3 before writing the font and renderer. That decision moves resident memory more than anything else.
