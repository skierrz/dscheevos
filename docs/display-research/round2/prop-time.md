# Proposal: TIME. Use the game's idle time wherever it really falls

Proposer angle: time. Instead of a fixed bottom band, show the indicator in idle time the game really has, per game. Read-only analysis: no repo edits, no emulator runs.

Analysis scripts (all in the scratchpad, `round2/time/`):
- `an.py`, `all.py`, `best.py`, `win.py`, `gaps.py`, `per.py`: census frames.csv;
- `ras.py`: survey runs3 JSON;
- `best.txt`: the per-game window table.

Units:
- Census cycles are 1/134 MHz units, so a frame is 2,240,760 units and a line is 8,520 units (about 4,260 ARM9 cycles).
- GBATEK "DS Display Dimensions / Timings" (gbatek.txt:7822-7825): 355 dots per line, of which 99 are H-Blank (594 bus cycles at 33.5 MHz), and 263 lines per frame.

---

## 0. What the data says (read this first; it changes the problem)

### 0.1 The census's "last wait" mixes three different things

`wait_line` is the VCOUNT at which the last run of halts began. A run is broken when more than 4,000 units of **non-IRQ-mode, non-BIOS** code run (melonDS `origin/dscheevos-wait-census:src/ARM.cpp:825-831`; ARM.h:395-400). `wait_run` is the non-IRQ code counted since then. That rule mixes three things:

1. **Real late game work** (what the strip must avoid).
2. **Interrupt work that runs outside IRQ mode.** Nested or SYS-mode handlers, and the idle loop re-entering `OS_Halt` after every wake, are counted as work.
3. **nds-bootstrap's own busy-waits** (CE9 card reads).

Evidence that (2) and (3) are large:

**Every-line H-Blank games show a fake "busy at the bottom".**
- The survey has the ARM9 H-Blank IRQ on all 263 lines, every frame (runs3 `hbl_lines`) for:
  - Animal Crossing, Kirby Super Star Ultra (KSSU), Pokémon Black, Mario Party DS, Kirby Canvas Curse, Trauma Center 2;
  - Starfy, on 65% of frames.
- In the census, the OS_Halt games among them have:
  - `wait_line` piled at 188-191 (Black 189-191 on 95% of frames, KSSU 188-191 on 86%, Mario Party 189-191 on 67%, Nintendogs 190-191);
  - `wait_run` that saws up to the 4,000 cap (KSSU p50/p90 1318/3954, Black 1366/2732, Mario Party 2590/3900);
  - yet 160-190 lines of halt earlier in the same frame (KSSU median 171, Black 177, Mario Party 186: `all.py` column `midP50`).
- **The control case:** Kirby Canvas Curse has the same every-line H-Blank IRQ but waits in the BIOS, whose re-halt loop runs at ≥ FFFF0000 and isn't counted. It shows `wait_line` 195-203 (waiting from inside VBlank) on 87% of frames.
- So for these games, "busy until 189" is most likely the counting rule, not game work. **Unproven** until the thread-aware log below exists.

**Periodic CE9 busy frames.** NSMB, SM64DS, Clubhouse, WarioWare DIY and MvDK2 all show 2-3 non-waiting frames every **14-15 frames** (gap-start spacing 15 or 14 in more than 95% of cases: `per.py`).
- On those frames the ARM9 PC at VBlank is often 023BFxxx, nds-bootstrap's CE9.
- Busy time is about the whole frame (median 2,232,584 of 2,240,760 units in NSMB, against 919,618 on good frames).
- These are card-read busy-waits, or a periodic bootstrap task, at about 4 Hz. Whether they come from the game's streaming, the census input script or nds-bootstrap itself is **unknown**. They matter a great deal (see 0.3).

### 0.2 Where the idle actually falls (class A)

Per-frame census, last wait only.
- "earlier halt" = halted time in the frame minus the last wait's length. It is a lower bound on idle that ended before the last wait.
- "runs≥120" = share of frames inside idle runs of at least 120 frames: 30 frames to qualify (R19) plus a 1.5 s notice.

| Game (run) | No wait in frame | Wait began in VBlank (whole picture idle) | Bottom 158-189 idle | 170-189 | 182-189 | Earlier halt ≥ 32 lines | runs≥120 at 182-189 |
|---|---|---|---|---|---|---|---|
| Mario Kart DS (race, 3,499) | 24% | 0% | 0% | 27% | 53% | 76% (median 149 lines) | 8% |
| Cory in the House (menus) | 13% | 43% | 48% | 48% | 58% | 38% | 31% |
| Diddy Kong Racing (menus) | 58% | 16% | 35% | 35% | 35% | 23% | 18% |
| Diddy Kong Racing (8k) | 87% | 5% | 11% | 11% | 11% | 7% | 6% |
| 999 (8k) | 23% | 0% | 54% | 61% | 70% | 75% (median 150) | 0% |
| Layton (gameplay) | 38% | 33% | 60% | 61% | 62% | 28% | 18% |

What it shows:
- **Mario Kart DS.**
  - The last wait begins at 160-191 on every race frame: 41% of frames at 160-175, 35% at 176-191.
  - The pattern is a strict period of 5 (`###..###..`): 3 frames idle by 182, then 2 not.
  - 149 lines of halt per frame sit earlier. In a 3D game these are most likely geometry-FIFO or DMA waits: a thread blocked on GX, which the red team's B1 says is **not** idle for the strip (`red-team-zero-lag.md`; display-library 11.4 step 2, `GXSTAT` bit 27).
- **Cory and Layton** are two-mode: whole frames idle (scenes) or late work (play).
  - Top-edge and bottom-edge cover the same frames.
  - Their gaps are mostly single frames (Layton 103 single-frame gaps in 5,000 frames).
- **999.**
  - `wait_line` is spread evenly over 64-191, changing frame to frame.
  - The longest idle run at any bottom window is 36-38 frames, so **0%** of frames sit in runs long enough for a notice.
- **Diddy Kong Racing.** Races are CPU-bound: 87% of 8k intervals have no halt at all. No time-based display is possible there.

### 0.3 Gaps, not averages, decide "no flicker"

Many games idle on 70-90% of frames but almost never for 120 frames in a row:

| Game | Bottom-band idle frames | In runs ≥ 120 |
|---|---|---|
| NSMB | 70% | 0% |
| SM64DS | 78% | 0% |
| Clubhouse | 67% | 0% |
| WarioWare DIY | 73% | 0% |
| MvDK2 | 71% | 0% |
| Metroid Prime Pinball | 70% | 0% |
| PMD Explorers of Sky | 61% | 0% |
| Rhythm Heaven | 68% | 4% |

The gaps are periodic: 2-3 frames every 14-15 (CE9) for the first five, and every 3rd frame for PMD Explorers of Sky (`##.##.`). Under "no flicker", a fixed strip in these games shows nothing, whatever the averages say. **This is the largest hidden class, and it isn't on the brief's list.** It is "class G: periodic busy frames".

---

## 1. Data the census lacks, and how idle-coverage's runner could log it

The census logs only the last wait and can't tell thread code from interrupt code. For a time-placed display we need, per frame:

1. **A thread-aware busy bitmap.**
   - Per line (263 bits = 33 bytes per frame), set a bit when an instruction runs that is not in an exception mode, not in the BIOS, and whose current SDK thread isn't the idle thread.
   - The current thread is `OSi_ThreadInfo.current`. Its address comes from `OS_GetCurrentThread`'s literal pool by signature, or from the module's halt hook, which knows the idle thread's `OSThread*` when it first halts.
   - Interrupt work (IRQ mode, or SYS-mode nested handlers while the idle thread is current) goes to a **separate** bitmap.
   - Runner change: in `ProfileInstr` (ARM.cpp:803 ff.), replace the `CensusSeqRun` rule with `cur = ram32(threadInfoCurrent)` per instruction, or cache it on mode change.
2. **The cause of each wake**: for every transition out of the idle thread, the line, `IE & IF`, and the thread it switched to.
   - This gives "which IRQ sources ever wake a game thread", per source. Mechanism T4 depends on it.
3. **What a halted game thread is waiting on.** At each halt with a game thread blocked, record:
   - `GXSTAT` bit 27 (geometry FIFO busy) and bits 24-26;
   - DMA 0-3 and NDMA busy bits;
   - an IPC/PXI request in flight;
   - CE9 `readOngoing`.

   This separates true idle from B1 waits (geometry, DMA, ARM7).
4. **Frames where the main thread is already in `OS_WaitVBlankIntr` but other threads still run.** Log the line where the thread that calls `OS_WaitVBlankIntr` (by signature) enters it. This sizes the "frame done but background thread runs" case (rejected below, but worth knowing).
5. **CE9 busy frames.** Log CE9 entry and exit lines (PC range 023BF000-023C0000 in DS mode) and the request's ROM offset. That answers whether the 4 Hz gaps are game streaming, the script, or nds-bootstrap.
6. **Re-run every-line H-Blank games** (KSSU, Black, Mario Party, Nintendogs, Animal Crossing, Starfy, Trauma Center 2) with 1-3, plus **`--thumb-cycle`** (eval-speed's runner) so slack isn't doubled on Thumb I-cache hits.

Output: per game, a "free-line map". For each line, the share of frames where no game thread and no blocking wait is active from that line to VBlank, plus the run-length distribution per candidate window. `best.py` already does this for the last-wait data.

---

## 2. Mechanisms, ranked

### T1. Per-game window inside the chosen edge band (fixed per game, chosen from the game's own map) — rank 1

**How pixels reach the player.** It is the existing P2b/11.4 path unchanged, except that the entry line `S` and height `N` come from a per-game table (allow-list entry), not a constant:
- NDMA3 is the H-Blank counter (startup 07h, gbatek.txt:27964), armed from the halt hook.
- The entry IRQ at the end of line `S-1` checks the facts (11.4 step 2).
- NDMA0 runs as the Main Memory Display FIFO (startup 09h).
- `STRB` writes DISPCNT byte 2 = 3; the exit is at the end of line `S+N-1`, with `S+N-1 ≤ 189` for a bottom placement.

The table also records the game's raster IRQ lines, so the window avoids them.

**Game state touched.** Same as P2b: R1/R20 (DISPCNT byte 2 for N lines), R4 (dispatcher), R5, R15. Nothing new.

**Cost in working time.** Same as P2b, bounded by X (section 4). Moving `S` doesn't change X.

**Who it serves (data).**
- **Mario & Luigi: Bowser's Inside Story (class B by the survey).**
  - It has a VCOUNT IRQ at 168 (and 192) on 64% of frames (runs3 `vc_lines`), and its last wait begins *at* 168 on 43% of frames: the 168 IRQ's short work, then idle.
  - Window 170-189 (N = 20): idle on 84% of frames and 47% in runs ≥ 120, against 41% and 30% for 158-189.
  - Window 182-189 (N = 8): 86% and 64%; 8k run 93% and 71%.
  - BIS moves from "no" to "yes".
- **Clubhouse Games (class B by the survey).**
  - Its only raster IRQ is VCOUNT at 191 (survey.md:118 row).
  - 191 is outside 158-189 when K = 2, so it was a false positive of the `bottom_band.py` band test, which used 156-191.
  - It still fails on its 14-15-frame periodic gaps (0.3); see T5/T6.
- **Trauma Center 2.** Window 182-189: idle on 59%, 15% in runs ≥ 120, against 39% and 0% for 158-189. That is partial, and it is also an every-line H-Blank game (T4).
- **Class A (Mario Kart DS, Cory, Layton, DKR, 999).** Little gain: their late work reaches 182+ on the bad frames.
  - Mario Kart DS at 182-189: 53% of frames but only 8% in runs ≥ 120, because of the 3-of-5 period.

**Risks.**
- A smaller N fits fewer characters. N = 8 is one text line with no badge; `dsd_render` needs an 8-line layout.
- Late-thread timing varies by level (survey coverage is the first minutes).

**Experiment.** No new hardware test is needed beyond H10/T1 (display-hw-tests.md) at `S` = 170 and 182.
- Re-score the survey with the exact window [S-1, S+N-1+K].
- Pass: BIS strip shown on ≥ 60% of gameplay frames in melonDS `--thumb-cycle`, with zero aborts mid-strip.

### T2. Per-notice placement (chosen when a notice starts, frozen until it ends) — rank 4 (minor)

At notice start, pick the highest-coverage window from the last 120 frames' map within the user's edge band. Keep it for the whole notice; re-choose only between notices.
- **No flicker,** because the position never changes while visible.
- It helps only games whose mode changes the best window (Cory: whole-frame idle in scenes, bottom 182-189 in play).
- **The data says the gain is small:** for every class A game, the frames where a higher window is idle are a subset of the frames where 182-189 is idle (`win.py`).
- **Per-frame movement is rejected:** a notice that jumps lines is flicker by the owner's definition.

### T3. Top edge on frames the game finished inside VBlank — rank 3 (placement option, not coverage)

Section 11.1 said the top edge "almost never" qualifies. **The census says otherwise.** The game finished its whole frame inside VBlank (the last wait began at lines 192-262) on:

| Game | Frames | In runs ≥ 120 |
|---|---|---|
| Tetris | 92% | 77% |
| Digimon | 91% | 72% |
| Phoenix Wright | 91% | 82% |
| Kirby Canvas Curse | 87% | 77% |
| Hotel Dusk | 85% | 85% |
| Ghost Trick | 85% | 73% |
| TWEWY | 88% | 47% |
| MvDK Mini-Land Mayhem | 87% | 70% |
| Yoshi's Island DS | 84% | 63% |
| M&L Partners in Time | 78% | 63% |
| PMD Blue | 77% | 71% |
| Club Penguin EPF | 75% | 40% |
| Platinum | 74% | 55% |
| SoulSilver | 71% | 54% |
| Peggle | 71% | 37% |

(`best.txt`.) **24 of the 66 census runs** finish in VBlank on ≥ 40% of frames.

**How pixels reach the player.**
1. **Entry at line 261-262.** The decision can be the NDMA2 V-Blank-delay IRQ of the original P2 or an H-Blank count, armed from the halt hook during VBlank. Since H-Blanks aren't counted during V-Blank (startup 07h), the cleaner entry is NDMA2 startup 06h plus an interval, as in P2 step 1. It needs the same facts as 11.4, in particular "interrupted PC in the idle code" and "no pending game IRQ".
2. The FIFO is armed at that point and **starts at the next frame, as GBATEK states**. The H10 question (mid-frame arming) doesn't arise: this is the one variant GBATEK documents directly.
3. Exit by H-Blank count N at the end of line N-1.

**Game state.** R1 (byte 2 for lines 261..N-1) and R2. Since the game is halted from VBlank until our exit, the R2 save path needs a game IRQ during the strip; those abort it.

**Cost.** Same X as the bottom strip.

**Who it serves.** The owner's "edge = top" placement setting, on the games listed above. It adds **no** class A coverage: Cory 43% (bottom 48%), Layton 33% (60%), DKR 16% (35%), Mario Kart DS 0%, 999 0%.

**Risk.** A game whose VBlank work sometimes runs past line 0 shows a per-frame dropout. The R19 gate handles it, and the runs data already prices it in.

**Experiment.** display-lab H10-top variant.
- Pass: clean banner on lines 0-31 with the FIFO armed at 261, and the T1 slack probe with zero lag difference at the same k as the bottom strip.

### T4. Abort on wake, not on IRQ (continue through game IRQs proven not to wake a thread) — rank 2

**Idea.** The P2b rule "abort on any game IRQ" is stricter than the real condition, "a game thread becomes runnable". The every-line H-Blank games take 263 IRQs per frame, so under the IRQ rule they can **never** have a strip (`ras.py`: no visible line is free of H-Blank IRQs in any of the 7 titles). Dodging between two raster IRQs is impossible: they are one line apart, every line, every frame. Moving the strip to raster-free lines is impossible for the same reason.

**Mechanism.** In the patched dispatcher (R4), while the strip is live:
1. **IRQ sources that have ever woken a game thread this session abort at entry,** as today. The set is learned per source (IE bit) by the module from idle:
   - after each IRQ return, did the idle thread resume, i.e. re-enter the halt hook, before any other thread ran?
   - Seed it from the thread-aware census (section 1, item 2).
2. **Sources that have never woken a thread** (e.g. a raster H-Blank handler that only writes registers) are passed to the game's handler with the strip still live.
3. **After the handler returns,** read the SDK's reschedule flag (`OSi_ThreadInfo.isNeedRescheduling`) at the patched dispatcher's exit, before `OS_IrqHandler_ThreadSwitch`.
   - If set, abort before the switch: mode back to 1, NDMA0 off, NDMA3 off. Then let the switch happen.
   - This makes the first-time wake safe too.
4. Our own NDMA IRQs are handled first in the dispatcher, before the game's bits. Our exit `STRB` is never queued behind a game H-Blank handler, so the strip can't spill onto line S+N.

**Game state touched.**
- R4 grows: a second hook point at the dispatcher's reschedule check, by signature in the same SDK function.
- Nothing else new. The game's H-Blank raster writes on strip lines change registers for lines we cover; they resume on the next line. The survey shows no legacy DMA writing DISPCNT A in the band (survey.md:390-394).

**Cost.**
- **Working time.** The game's own non-waking handler runs while the FIFO competes for main RAM. That is interrupt work during idle, not frame work, unless the handler is on a thread's critical path, which by definition it isn't (it didn't wake one).
  - The one real risk is **raster timing**: a slowed handler could finish its register write after H-Blank ends (594 bus cycles), tearing one line *below* the strip or on engine B. Starfy writes engine-B BG0 scroll from H-Blank (survey.md:299).
- **Bound.**
  - Per IRQ: one `tst` plus one flag load (~4 cycles) while the strip is live.
  - Per aborted frame: the abort (~20-60 cycles) happens before any game thread instruction.
  - X is unchanged; see section 4.

**Who it serves.** The every-line H-Blank games: Animal Crossing, KSSU, Pokémon Black, Mario Party DS, Nintendogs, Trauma Center 2, Starfy, and Canvas Curse once the BIOS hook exists.
- They are mostly halted: busy 37-48% per census, with 160-190 lines of earlier halt.
- If section 1 confirms the 188-191 "late wait" is SYS-mode handler time, T4 alone moves them from 0% to roughly their halt share.
- **Unquantified until that census exists. This is the biggest open number in this proposal.**

**Experiments.**
1. **Thread-aware census** (section 1) on KSSU, Black, Mario Party.
   - Pass: game-thread-free from line 158 to VBlank on ≥ 60% of gameplay frames.
   - Kill: game threads run at 158-189 on > 50% of frames. Then the late work is real and these games join class A.
2. **melonDS `--thumb-cycle` raster check.** With the strip on, log for every game H-Blank handler while live the dot position of its last display-register write.
   - Pass: every write lands before dot 355 of its line (still inside H-Blank) in 10,000 frames.
3. **DSi display-lab "H-Blank neighbour".** A stand-in game with an every-line H-Blank handler that writes BG0HOFS from main-RAM code; strip on/off; photograph or capture lines S-1 and S+N.
   - Pass: no shifted line, with the handler's code in uncached main RAM (worst case).

### T5. Notice scheduler that respects gaps (no partial notices, ever) — rank 1 (with T1)

This is the "no flicker" rule turned into policy. It applies to every strip variant.
- A notice starts only when the window has been clean for **W** frames; R19 used 30. Choose W per game from its run distribution, so that P(a run of length ≥ W + D continues | clean for W) ≥ 99%. D is the notice length.
- **The first miss ends the notice.** It goes to the menu's Missed list and the LED cue fires. A strip that is aborted mid-draw also ends it: that frame shows a partial banner once, which is the one visual compromise left, and its rate must be measured.
- **Phase lock for periodic games.** Where gaps are strictly periodic (NSMB/SM64DS/Clubhouse 2-3 frames every 14-15; Mario Kart DS 2 every 5; PMD EoS 1 every 3), a notice can't span a gap without flicker. **No placement fixes this.** The only levers are:
  - remove the gap's cause (CE9 busy-waits → section 6);
  - shorten the notice to fit inside a gap-free run. 11-12 frames (0.2 s) is too short to read;
  - give these games LED plus menu, with a strip only in their long gap-free stretches.
- **Missing-frame-tolerant UX (rejected).** Holding the notice on screen across a gap needs the strip on a busy frame. That costs bus time in working time, which the zero-lag ruling rejects.

**Data (runs ≥ 120 at the game's best bottom window)** (`best.txt`):
- **≥ 60%:** Phoenix 91, Platinum 90, WarioWare Touched 88, Club Penguin HR 87, Cooking Mama 87, Hotel Dusk 85, PvZ 85, SoulSilver 85, Spore 83, TC1 82, Canvas Curse 82, MvDK MLM 84, Kirby Mass Attack 79, Sonic Rush 77, Yoshi 77, PMD Blue 78, Digimon 72, PiT 70, Pac'n Roll 73, Zelda ST 69, Club Penguin EPF 69, Zelda PH 64, BIS (T1) 64, Squeak Squad 62, Peggle 62, TWEWY 61, Children of Mana-8k 59.
- **Periodic-gap class at 0-8%:** NSMB, SM64DS, Clubhouse, WarioWare DIY, MvDK2, Metroid Prime Pinball, PMD EoS, Rhythm Heaven, 999, Mario Kart DS.

### T6. Every-Nth-frame or split-across-frames display — rejected

- **Showing on alternate frames** (30 Hz on/off) or only on idle frames gives a 30 Hz or irregular on/off of a high-contrast banner.
  - The DS/DSi TFT is driven at 59.83 Hz (gbatek.txt:7825). DS games use exactly this 30 Hz alternation for "flicker transparency", and it reads as shimmer. Persistence of vision doesn't hide it, and the LCD's response doesn't integrate it away.
  - **This is flicker of our own notice. The rule bans it.**
- **Splitting a banner spatially across frames** (top half on even frames, bottom half on odd) is the same thing.
- **A slow deliberate blink** (≥ 250 ms on, ≥ 250 ms off, each phase all-or-nothing, so an "on" phase needs 15+ clean frames) is a UX animation, not frame flicker.
  - It fits the periodic-gap games: NSMB's clean runs are 12-13 frames, which **doesn't** fit 15; Mario Kart DS's 3 frames doesn't fit either.
  - It would only help games whose gaps are rarer than every 15 frames, and those games already pass T5.
  - **No coverage gain. Rejected** rather than offered as a residual.

### T7. The game's own blank, transition and fade frames — rank 5 (queue drain only)

- **Uniform or forced-blank frames:** P3's black-frame card. Survey "Black ≥ 30f per 5 min" is 0-3 for most titles (Starfy 51 counting fade 16). Load frames carry card reads, which stand the strip down.
- **Fades:** master brightness also applies to mode 3 (gbatek.txt:7988), so the strip fades with the game (R10). It is deferred while the factor is ≥ 12.

**Use.** Drain queued unlock notices on the first clean idle stretch after a transition. It adds no coverage.

### T8. Idle signals from idle-coverage's hooks (classes C and D) — rank 1 for C, 3 for D

The display needs one fact at its entry IRQ: **the interrupted context is a wait that ends only on an IRQ we can see.**

**C, BIOS `VBlankIntrWait`.** idle-coverage hooks the wrapper (`movs r2,#0; svc 5; bx lr`, idle-census.md).
- **Signal:** a hook-side token `in_bios_wait = 1` before the `svc` and 0 after, plus the fact check "interrupted PC ∈ BIOS IntrWait halt loop (FFFF0xxx) **and** token set". The PC check defeats the stale-flag problem (B2).
- **The arming point** is the wrapper hook itself (the wait start). It arms NDMA3 to line S-1, as the halt hook does.
- **Abort** is the same as T4. The BIOS loop re-halts after non-VBlank IRQs without running thread code. Our NDMA IRQ must not set the BIOS IntrWait flags (DTCM+3FF8), so the BIOS re-halts after it: our handler leaves that word alone.
- **Who it serves.** Waiting by line 156 (census): Sonic Rush 89%, Canvas Curse 87%, M&L PiT 85%, Pac'n Roll 85%, Children of Mana 73%, Super Princess Peach 71%, MP Pinball 70%, Pokémon Ranger 58%, RE:DS 54%. In runs ≥ 120: Canvas 82, Sonic Rush 77, Pac'n Roll 73, PiT 69, SPP 48, RE 31, CoM 10-49, Ranger 15, MPP 0 (periodic).
- **Cost:** the same X as oshalt.

**D, busy poll loops (Golden Sun: Dark Dawn, Code Lyoko).** The census shows 0% halts (busy 92% and 99%).
- **Signal:** idle-coverage identifies the poll loop: PC range, what it polls, and the compare constant.
- **Fact check:** interrupted PC ∈ the poll loop's range.
- **Two cases:**
  - **(a) Loop polls a RAM flag set by an IRQ handler.** Abort on that IRQ (the flag-setter can't be told apart from other sources). The loop's own load hits the D-cache (the IRQ handler wrote it on the same CPU), so the strip doesn't slow the loop.
  - **(b) Loop polls VCOUNT or GXSTAT.** No IRQ ends it, so the strip must end before the loop's target line: exit line ≤ target-2, taken from the constant.
- **Cost:** at most one contended main-RAM access per loop exit, if the loop isn't cache-resident (≤ ~20 cycles, in X).
- **Risk:** Golden Sun runs its "idle" inside a task scheduler. There may be no distinguishable poll loop, only task dispatch. Then D-GS can't be served by time; see section 5.

---

## 3. Cost table summary

| Mech | Pixels | Game state | Worst-case cost in working time | Classes |
|---|---|---|---|---|
| T1 window | NDMA FIFO, mode 3, lines S..S+N-1 | R1/R20, R4, R5, R15 | X (§4) | B-VCOUNT (BIS, Clubhouse), A partial |
| T3 top | same, lines 0..N-1, FIFO armed at 261 | R1, R2 (aborted on IRQ), R4 | X | placement setting |
| T4 abort-on-wake | same | R4 + a reschedule-check hook | X; raster-timing risk on the line after the strip | B every-line H-Blank (AC, KSSU, Black, Mario Party, Nintendogs, TC2, Starfy, Canvas) if §1 confirms |
| T5 scheduler | policy | none | 0 | all (no flicker) |
| T8-C | same | idle-coverage wrapper hook | X | C (9 census ROMs; ~45-50 RA games) |
| T8-D | same | idle-coverage loop hook | X + ≤ 1 contended load per loop exit | D, if a poll loop exists |

---

## 4. The bound X: how to make it as tight as possible

X is the strip's worst-case cost per frame inside the game's working time. Make every term per-armed-frame and at most one per frame, and make the always-on part zero:

1. **No session-wide `tst`.**
   - Install the strip branch only while armed. Arming (in idle) swaps the dispatcher's branch target; disarming restores it. Then unarmed frames, including every frame with no notice, cost **0 cycles**.
   - The swap target can be a patched branch inside the dispatcher's ITCM copy (code patch, R4 family, with D-cache clean and I-cache invalidate of that line from idle), or the vector at DTCM+3FFC (game DTCM write; owner's R4 choice).
   - That beats 11.4's per-IRQ `tst` for all IRQs: every-line H-Blank games take 263 IRQs per frame, about 263-526 cycles per frame session-wide.
2. **At most one arm per frame.** It is armed in the first idle stretch that starts before line S-1 and never re-armed in the same frame. The disarm cost (~20-60 cycles) therefore happens at most once per frame, and only on frames where the game woke before S. With T4, only on wake, not on every IRQ.
3. **The NDMA3 counter runs only from arm to entry.** If the game wakes first, the disarm stops it.
   - While running it makes one 1-word transfer per H-Blank to its own FDATA I/O register. That is ARM9 bus arbitration of a few cycles per line, to be measured in H11.
   - The tighter option: arm NDMA3 for the whole count only when the idle stretch starts within 8 lines of S. Otherwise re-arm from the next halt-hook pass. That needs IRQ returns to re-enter the halt hook; they do for OS_Halt and BIOS waits.
4. **Abort cost ≤ a fixed instruction sequence,** run before any game handler (wake sources) or before the thread switch (T4):
   - `STRB` DISPCNT;
   - NDMA0CNT = 0;
   - NDMA3CNT = 0;
   - restore the branch.

   That is about 8 instructions in ITCM, ≤ 30 cycles.
5. **Bus residue after the abort.** NDMA0 may finish its current 16-word burst after the disable. Bound it as ≤ 1 burst (~16 main-RAM reads), measured in H11.
6. **R21 (ARM7 replies slowed by FIFO contention)** is the only term not bounded on the ARM9.
   - Bound it with an ARM7 timestamp probe in the display-lab: PXI reply latency with the strip on vs off, 10,000 samples.
   - Then gate: no strip while a PXI request is in flight. The facts check already lists "ARM7 reports no save I/O or wireless".

**Resulting X.** Per frame, for frames with a notice and an early wake: ≤ disarm (≤ 60 cycles) + one burst residue + the NDMA3 per-line arbitration for the lines between wake and disarm (0 with item 3's late arm). Otherwise 0.

That is about 0.005-0.01% of a frame (frame = 1.12 M ARM9 cycles), on frames that show a notice and whose idle broke early. **T1 slack probe** (display-library 11.4) measures it; run it for each variant (bottom, top, mid-window, T4 continue-through, BIOS).

**Pass:** zero lag-frame difference at k = 200 cycles over 10,000 frames for each variant.

---

## 5. Who can't be served by time, with evidence (pending section 1)

1. **Mario Kart DS race** (class A).
   - Every race frame has thread work until line 160-191 (0% idle by 158, 2% by 160: census, 3,499 gameplay intervals).
   - Idle at 182-189 comes on a strict 3-of-5 frame period, so no notice-length run exists (8% of frames in runs ≥ 120, all in non-race stretches).
   - The 149 lines of earlier halt are most likely GX/DMA waits (B1), not idle (to confirm with section 1, item 3).
   - Top edge: 0%. Mid-frame window: only if section 1 shows that earlier halt is true idle with no GX/DMA busy. **Unlikely for a 3D racer.**
   - **Levers tried:** fixed bottom, smaller N, per-game window, top edge, per-notice placement, every-Nth-frame, transition frames. All fail on data or on the flicker rule.
   - **Remaining:** LED/camera LED plus the menu. P4 isn't available because the bottom screen shows the map.
2. **Diddy Kong Racing races.** 87% of intervals have no halt (8k run). There's no time to use. Same levers, same outcome.
3. **Periodic-gap games (new class G).** NSMB, SM64DS, Clubhouse, WarioWare DIY, MvDK2, Metroid Prime Pinball, PMD EoS, Rhythm Heaven, 999.
   - 61-78% of frames idle at the bottom, but gaps every 3-15 frames, so 0-8% in runs ≥ 120.
   - **Curable only if the gaps aren't the game's.** For the first five, the gap frames' PC sits in nds-bootstrap's CE9 (023BFxxx) with the frame fully busy. If those are CE9 busy-waits on SD reads, changing CE9's wait from busy-poll to halt doesn't help: the strip must still stand down during reads (FIFO starvation, B2, and ARM7 SD-to-RAM contention).
   - **The only cures are upstream:**
     - CE9 prefetch/cache sizing so the 4 Hz reads stop (nds-bootstrap's ROM cache; perf session);
     - or proof that the 4 Hz cadence is the census input script, not the game.
   - **Until then:** LED + menu, with notices only in their long gap-free stretches.
4. **Class E (POWCNT1.15 flipped per frame).**
   - Engine A drives each physical screen on alternate frames. The other frame shows engine B's display of a capture, and capture source A is "BG+3D+OBJ" (gbatek.txt:8134-8151), which excludes the mode-3 strip.
   - So the strip appears on each screen at 30 Hz. That is flicker (T6).
   - **No time lever exists.** Hand it to the space or other-mechanism proposers.
5. **Golden Sun: Dark Dawn,** if idle-coverage finds no poll loop (T8-D risk).

**Not unservable but unproven:** the every-line H-Blank class B games. T4 serves them **if** section 1 shows their late "wait" is SYS-mode handler time. That is the single most valuable experiment in this proposal.

---

## 6. Ranking and the combination

1. **T5 + T1** (per-game window, gap-aware scheduler). Serves the ~25 "≥ 60% in runs" titles, plus BIS via a new window, plus Clubhouse after its gap cause is fixed.
2. **T8-C** (BIOS wait signal). Adds the 9 BIOS ROMs (~9% of the RA library), most at 50-90% coverage.
3. **T4** (abort on wake). Adds 7-8 every-line H-Blank titles, conditional on the census.
4. **T3** (top edge). Serves the owner's top-edge setting on ~24 titles.
5. **T8-D**, **T2**, **T7**: small additions.
6. **T6:** rejected.

**Does the combination reach 100%?** No. With T1+T3+T4+T5+T8, the time angle serves:
- every OS_Halt or BIOS game whose idle is long and unbroken;
- the H-Blank class if confirmed.

It **can't** serve, by time alone:
- Mario Kart DS and DKR races (no idle at the bottom, periodic);
- class E (30 Hz by construction);
- class G (periodic CE9 gaps) until nds-bootstrap stops the 4 Hz busy frames;
- possibly Golden Sun DD.

Those need a mechanism that costs nothing in working time *and* doesn't depend on idle: the LED tier, the menu, P4 when the other screen is blank, or (3DS) TwlBg. Or they need the owner to accept a measured, nonzero working-time cost.

**Decisive experiments, in order:**
1. **Thread-aware census** (§1, items 1-3) on KSSU, Black, Mario Party, Mario Kart DS and 999. It decides T4's class and confirms or refutes "Mario Kart DS unservable".
2. **CE9 4 Hz gap cause** (§1, item 5) on NSMB and SM64DS. It decides class G.
3. **Display-lab:** H10 bottom at S = 170/182, H10-top, the T1 slack probe for each variant, and the T4 H-Blank-neighbour test.
