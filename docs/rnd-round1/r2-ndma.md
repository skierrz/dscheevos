# Lane R2-NDMA: decision report

## 1. Findings

**F1. NDMA can't do the work that costs us. Evaluation is the bottleneck, and NDMA can't reach it.**
- On MKDS, rcheevos plus read_memory is about 801k of 873k cycles per frame (profile-mkds.txt:2,32).
- Snapshot copy plus pointer chains is 2,818 + 2,650 cycles, about 0.6% (profile-mkds.txt:45-46).
- Evaluation D-misses fall almost entirely on set data. read_memory, the only evaluation code that reads the snapshot, takes about 93 of about 5,700 D-read misses a frame on MKDS (about 1.6%) and about 58 of about 1,460 on Kirby (about 4%). This is a proxy; region attribution (M2) was never run.
- MKDS streams about 180 KB of set data a frame through a 4 KB D-cache (BRIEF.md:39-40, TEAM.md:71-72).

**F2. Hardware limits, verified in GBATEK. Each one closes off an idea.**
- **No gather.** NDMA has no scatter-gather and no descriptor chaining. Each channel moves one linear source to one linear destination, and an immediate start ends after one logical block (gbatek.txt:27933-27955, 27993-28004).
  - Self-programming chains only work through the 1Ch register stride, which reaches at most the other 3 channels, so 3 runs.
  - Event-repeat chaining would need an event we own. Only V-Blank (once a frame) and H-Blank (once a line) qualify; Timer0-3, cart and GX FIFO belong to the game (gbatek.txt:27956-27972).
  - Measured sets need 12-229 runs, so neither route covers them.
- **No TCM, no cache.** DMA can't reach DTCM or ITCM, and it bypasses the D-cache (gbatek.txt:12762-12768, 94560-94567). A "TCM prefetch" or "cache warming" is impossible.
- **rcheevos is unmodified and walks absolute pointers.** Set data can't be paged through an NWRAM window, and MBK is locked once SCFG_EXT9 bit 31 is cleared (bootloaderi main.arm9.c:436).
- **Pointer chains need dependent reads.** NDMA can't follow AddAddress chains, and those chains are what Platinum capture (3,888 AddAddress) and HG/SS capture (17,306) consist of (set-stats.tsv).
- **The CPU stalls during DMA.** The ARM9 keeps running during a DMA only while it hits TCM or cache (gbatek.txt:12766-12774). Since rcheevos is miss-bound, the only real overlap is while the ARM9 is halted in wfi, which is the game's idle time.
- **Physical blocks are never split** (gbatek.txt:27920-27923). An NDMA still running when the game's IRQ fires delays the game's first bus access, which the game can observe.

**F3. The state nds-bootstrap leaves NDMA in.**
- DS mode sets SCFG_EXT9 to 0x0301C000: NDMA access on (bit 16), extended IRQs off (bit 8 = 0), then locked (main.arm9.c:429-436).
- So there's no NDMA completion IRQ; completion has to be polled from the busy bit.
- Stock NTR runs with NDMA disabled (0x03000000, gbatek.txt:24313-24316).

**F4. Channel ownership. The task's "display uses 0/2/3" is partly stale.**
- CE9 uses NDMA0 (TWLSDK) or alternates NDMA0/NDMA1 by i%2 (non-TWLSDK ROM-in-RAM) during play (cardDma.thumb.c:230, 441-463).
- The colour-LUT engine uses NDMA3 (arm9_colorlut cardengine.c:411).
- Display uses NDMA0 (FIFO feeder, 09h) and NDMA3 (H-Blank counter). P2b drops NDMA2 (display-library.md:221-229, 406-413, 480).
- **NDMA2 is the only unclaimed ARM9 channel, and only once display adopts P2b.**
- CE9 zeroes all four channels on reset (misc.c:263-265). The ARM7 bank is separate (CE7 SD on slot 0, my_sdmmc.c:325-338).

**F5. Throughput and priority are unknown on DSi hardware.**
- Derived from NDS timings: main RAM to WRAM is about 3 bus cycles a word. Main to main is all non-sequential (gbatek.txt:12776-12782), about 20 bus cycles a word (derived, not a GBATEK statement), which is slower than a CPU line fill (23 bus cycles for 8 words).
- DSi NWRAM timing is undocumented.
- NDMA priority against legacy DMA and against ARM7 sound fetches is unknown (gbatek.txt:12759-12761, 27881-27882).
- melonDS can't answer any of this:
  - it halts the ARM9 for the whole NDMA;
  - it models no BCNT interval, physical block size, round-robin or ARM9/ARM7 main-RAM contention;
  - it charges 18 bus cycles a word main to main and 2 a word main to WRAM;
  - sources: DSi_NDMA.cpp:137-193, 218-246; DSi.cpp:2913-2917.
- A melonDS lag A/B would therefore "pass" without measuring the actual risk.

**F6. Coherency.**
- A watched line is dirty at VBlank in 88% of Platinum frames, 61% of Sonic Rush, 9% of Kirby and 1% of MKDS. Chain targets weren't counted (arm7-notes.md:62-73).
- Any non-ARM9 reader of game RAM needs a clean-by-VA plus a write-buffer drain first. The helpers already exist (start.s:237-297).
- A clean is functionally invisible to the game.
- One hazard is unverified: the game DMAs into a range that is still dirty in the cache and invalidates only afterwards, so our clean overwrites the DMA'd data. That hazard already exists from our eviction pressure.
- The per-line clean cost is undocumented.

**F7. Red-team corrections I accept:**
- **The arena write misses are not cache pollution.** The D-cache is read-allocate (gbatek.txt:7345), so arena stores don't evict set data. Kirby's about 1,203 DW misses a frame are write-buffer/uncached-store stalls.
  - melonDS has no write buffer and charges 18-19 cycles per store (plan.md:50), so part of that cost is an emulator artifact.
  - Both proposals' Kirby NDMA savings rested partly on this false mechanism and are overstated.
- **A "read-touch" of the destination doesn't make write misses hits.** Each touch is a line fill (about 46 ARM9 cycles) and evicts a line. That software lever is withdrawn.
- **Per-run reprogramming is 4 I/O writes (SAD, DAD, WCNT, CNT), not 7.** BCNT, FDATA and TCNT persist while the channel is disabled (gbatek.txt:28006-28010).
- **NWRAM isn't free.** nds-bootstrap's FNT/FAT already sits at 0x03700000 (memplan/proposal-safe.md:58), and colour-LUT uses NWRAM too. MPU coverage and cacheability of 0x037xxxxx are lever A's preconditions, not something this lane can assume.
- **The staging step defeats itself.** Proposal 2's "NDMA into NWRAM staging, then CPU copy into the slot" brings back the same write stream.

**F8. Red-team corrections I reject or qualify:**
- **"556k is unsourced":** it is the baseline's [KB] runner figure (all idle work per interval, overhead included).
- **"Kirby fits with 3.5x headroom" (plan.md:68):** this refers to evaluation only, 195k against 688k. Both statements hold.
- **"Platinum deficit of about 760k":** derivable as 1,527k set bench minus 762k w1024 (D, cold bench, no in-game profile).
- **The Kirby set identity (14763 vs 14700) is unresolved.** The round profile's run_chains at 14k cycles a frame suggests chains, which doesn't fit 14763's 0 AddAddress. Treat the Kirby run counts as indicative only.

**F9. Coarse-gap over-copy, the variant neither proposal costed.**
- Copy a few large spans, at 512 B to 4 KB gaps, into NWRAM while the ARM9 is halted. That cuts per-run programming to a handful of runs, so CPU cost per wake approaches clean + 4 writes + poll.
- Its real costs are bus occupancy during the game's idle (ARM7, sound, CE7 SD and game DMAs), which is unmeasurable in melonDS, and NWRAM footprint that competes with lever A and FNT/FAT.
- It still moves only direct-word capture, so the ceiling in F10 still applies.

**F10. Ceiling on ARM9 cycles saved per frame.** All figures are derived; none are measured.

| Game | Max NDMA-movable work | Basis | Gap to fit |
|---|---|---|---|
| MKDS | 0-2k (≤0.3%) | copy_snapshot 2.8k minus clean, programming and poll | about 550k (873k vs 319k w1024) |
| Platinum | 0-3k | 38 direct addresses; capture is chains | about 760k (D) |
| Black | ≤3k | capture 2.2k instructions in total | about 150k (848k vs 704k) |
| HG/SS, TWEWY, Digimon | about 0 | table overflow, live reads, or evaluation-bound | 3.5-15x |
| Kirby SSU | 0 to about 40k (D, unverified) | per-wake copy repeated 20-37 times; write-stall saving partly a melonDS artifact (F7) | none, already fits |

No game moves from "misses frames" to "fits". The only sizeable figure is on Kirby, which already fits. Kirby's real lever is software: don't redo the full copy on each of the 20-37 wakes a frame. That belongs to the capture lane.

## 2. Recommendation: not worth it. Build nothing and don't make a standalone lab ROM.

**Expected cycles saved per frame (all D, upper bounds):**
- **MKDS:** 0-2k.
- **Platinum:** 0-3k.
- **Kirby SSU:** 0 to about 40k, on a game that already fits, and most of it is reachable in software without NDMA.

**Why the cost is zero.** Nothing ships, so there's no new bus master, no new IRQ-masked window, no clean traffic, and no use of NDMA2. That leaves NDMA2 for display and NWRAM for lever A.

**Why not "experiment first".** The decisive question for this lane is "does it close a deficit?", and that is already answered on paper by F1, F2 and F10. No hardware result can change it: even infinitely fast, zero-contention NDMA saves at most about 0.3% on MKDS. Spending the owner's hardware-loop time on an NDMA-LAB would buy numbers this lane can't use.

**Two measurements do have consumers outside this lane.** Contribute them to existing test ROMs instead (section 3).

## 3. Disposition and hand-offs. There is no build plan.

**3a. Record the decision.** The rnd session owner does this; this worktree is read-only to me.
- Add an R2-NDMA entry to WORKLIST.md and evalspeed-team/TEAM.md: "Closed. Ceiling per F10; NDMA can't gather, can't follow chains, can't write TCM, can't prefetch into the cache."
- Reopen criteria:
  - (i) after the capture lane's software fix and the plan.md levers, some game that misses frames still spends more than 50k a frame on direct-word capture (C_sw, measured in melonDS with `run_cpi.sh --thumb-cycle --cache-model`);
  - (ii) display has ruled on NDMA2;
  - (iii) lever A's M1/M3 show NWRAM is mapped, cached-or-not as known, and has spare room beyond FNT/FAT.

**3b. To the capture lane** (capture-notes.md owner; nothing committed yet):
- **Repeated capture.** Kirby's capture is repeated on 20-37 wakes a frame (BRIEF.md:47-51, TEAM.md:81). Candidates: allocate the slot once a frame; move the repeat compare to once a frame in dsc_idle; recopy only on a dirty indication.
- **Do NOT add a destination read-touch** (F7).
- **Pass/fail, decided in advance:**
  - host tests check_snapshot/check_repeat and a byte-identical realsets digest pass;
  - melonDS C_sw drops;
  - ab.sh shows lag intervals equal between stock and achievements over 6,000 intervals.
  - That A/B also settles the open Kirby 172/6000 lag question.
- **Resolve the Kirby set ID.** The round profile is either 14763 or 14700; settle which before quoting per-set figures.

**3c. To display (dscheevos-43): add two tests to the planned H6/H11 hardware ROM** (display-hw-tests.md:17-33), which already sets up NDMA bus tests.
- **T3. ARM7 latency under ARM9 NDMA.** An ARM7 uncached main-RAM read loop, timed with the lab ROM's own timer, with and without ARM9 NDMA at physical blocks of 2^0, 2^3 and 2^7, plus 16 sound channels streaming from main RAM, compared through capture DMA.
  - The pass threshold must come from zero cost: no audio sample difference over 600 frames, and the worst single ARM7 access delay no more than one 2^3 block (about 24 bus cycles, D). Display owns the final number, since its FIFO NDMA is the actual user.
- **T2. CP15 clean-by-VA cost per line**, 128 clean lines against 128 dirty lines, plus the drain. It's a validity check if dirty costs more than clean.
- Both are also answers that arm7eval needs (T2 and T3).
- **Also tell display:** an NDMA2 claim by anyone besides display is withdrawn, and the task's "0/2/3" listing should read 0/3 under P2b.

**3d. To idle-coverage / arm7eval** (ARM7 placement isn't designed here):
- **A primitive:** a watched-line clean-by-VA plus drain at each halt, for any non-ARM9 reader of game RAM. It has to include chain targets, which arm7-notes.md didn't count.
- **The argument that it is safe:** a clean writes back only what the CPU wrote and leaves the lines valid. Our evaluation already evicts every dirty game line each frame by streaming about 180 KB, so a targeted clean is less disruptive than today's behaviour.
- **Add to their test list:** the hazard of a game DMA into a still-dirty range that is invalidated afterwards (F6).
- **Note for them:** CE9's NDMA0/1 card reads during play also load the bus. T3 bounds that.

**3e. nds-bootstrap card engines:** no change requested. The facts to keep on file:
- CE9 holds NDMA0/1 and zeroes all channels on reset;
- SCFG_EXT9 bit 8 stays 0 unless display's R4/R5 work lands. That work needs a pre-lock bootloader write plus the OS_IrqHandler routing patch.

## 4. Open questions only hardware can answer

1. NDMA priority against legacy DMA and against ARM7 sound-channel fetches. GBATEK lists both as unknown.
2. Whether BCNT's interval applies after each physical or each logical block, and whether the gap actually frees the bus for the ARM7, sound and game DMAs.
3. What NDMAGCNT round-robin "cycle selection" actually does.
4. DSi main-RAM NDMA throughput, main to main and main to NWRAM. GBATEK's figures are NDS-derived, and melonDS's 2 and 18 bus cycles are unvalidated.
5. Whether the ARM9 NDMA can write NWRAM-B/C mapped at 0x037xxxxx, and the NWRAM access latency.
6. CP15 clean-by-VA cost per line, for clean and dirty lines.
7. Whether SCFG_EXT9 bit 8 really gates IE/IF 28-31 in DS mode (display R5).
8. ARM9 IRQ latency when the IRQ arrives mid physical block.
9. Whether any game DMAs into a still-dirty range and invalidates afterwards (the clean-visibility hazard).
10. Whether DSi-enhanced TWL-SDK games drive NDMA channels themselves under nds-bootstrap. That needs a ROM-corpus scan for 0x040041xx writes.
11. Whether 3DS TWL mode behaves the same as the DSi for all of the above.

## 5. Summary for the orchestrator

R2-NDMA is closed as not worth it. NDMA can't gather, can't follow AddAddress chains and can't write TCM or the cache, so it can take on only direct-word capture: 0-2k cycles a frame on MKDS, 0-3k on Platinum, about 0 on HG/SS, TWEWY and Digimon. No game that misses frames moves closer to fitting.

The only real gain is on Kirby, at most about 40k a frame (derived, partly a melonDS write-stall artifact), and Kirby already fits. That gain goes to the capture lane as a software fix: stop recopying on each of the 20-37 wakes a frame, with no read-touch.

Hand-offs, with no standalone lab ROM:
- **Display:** add T2 (clean cost) and T3 (ARM7 latency and 16-channel sound under ARM9 NDMA) to its H6/H11 ROM. NDMA2 stays with display.
- **arm7eval / idle-coverage:** the clean-at-halt primitive and the DMA-into-dirty-range hazard go on their test list.