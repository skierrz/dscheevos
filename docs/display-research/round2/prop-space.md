# Round 2 proposal: space and engines

Angle: other places on the console to put pixels, and whether any of them costs nothing. Read-only work. The one new analysis is a re-read of the M1 survey's per-frame logs (`scratchpad/survey/runs3*/*.frames.csv.gz`, which sample DISPCNT A/B, POWCNT1 and DISPCAPCNT at line 0, from frame 600 onward). The scripts are `round2/space/an.py` and `an2.py`.

## 0. Summary

1. **The best new lever is a VRAM-display band ("P2v"): DISPCNT A mode 2 from a free bank A-D, used in place of the FIFO.**
   - It costs no main-RAM bus at all. That removes the FIFO's own problems:
     - R18/R21 (the ARM7 slowed by bus contention);
     - red-team-zero-lag's B1 (it slows waits for ARM7 replies);
     - underrun garbage, and the stand-down during card reads;
     - the hardware unknowns H1, H5 and H10.
   - **Because nothing is shared with the game's working time except two short IRQ entries, the band no longer needs the game to be halted.** It can run on busy frames (class A), during raster IRQs (class B), under BIOS waits (class C) and in games that never halt (class D).
   - The cost is a free bank. Whether games have one is the decisive unknown; the survey didn't log VRAMCNT. Experiment S1 settles it.
2. **Class E (screen swap every frame) can't be served flicker-free by any engine-A mechanism alone.** Engine A drives each screen only every other frame, so a band on it shows at 30 Hz.
   - The fix is a **capture stamp**: the same band pixels are written into the game's own capture bank after its capture ends and before engine B shows it.
   - That is a write to game VRAM. The game overwrites the bank itself one frame later and never reads it back (gated on capture source = A only). It is a residual the owner must accept.
3. **Engine B can't show anything without game state.** It has no FIFO, no VRAM display and no capture (`gbatek.txt:7982-7983, 8198`).
   - New data: P4's trigger, a sub screen showing backdrop only for 30 or more frames, **essentially never happens during play.** It occurred in 0 of the 27 titles that reached play, outside transitions (section 4).
   - P4 as designed therefore serves about nobody. Widening it to cover static sub screens means occluding and borrowing game layers, which is the path Bakkerrrs' hardware failures came from.
4. **Rejected, with reasons:**
   - **Display-capture compositing:**
     - it duplicates a frame at start, drops one at the end, and adds +1 frame of latency throughout;
     - its blending is additive only, so it can't draw a dark or opaque box;
     - it needs a free bank anyway, so P2v dominates it;
     - with the FIFO as source B it costs bus for all 192 lines.
   - **The 3D engine and rear plane:** its output reaches the screen only through the game's BG0 or through capture.
   - **Remapping a game bank to LCDC for the band lines:** the band's rows would be the game's tile data.
5. **Occlusion.** An opaque band over the game picture is not "damage": the game's memory and its next frame are untouched, and RetroArch's popups also sit over the picture. It is still a cost, so this proposal bounds it (section 7).
   - On engine A the minimum footprint is a **full-width band**: modes 2 and 3 replace whole lines.
   - Hence: no persistent on-screen challenge indicator (use the camera LED, plus a short band at prime and unprime), unlock notices of 3 s or less, progress of 2 s or less, and bands of 16 lines (compact) or 32 (full).

## 1. Facts this rests on

| Fact | Source |
|---|---|
| Display mode 2 shows a 256x192 direct-colour bitmap from the bank in DISPCNT.18-19. Engine A only. It bypasses BG/OBJ/3D/effects; master brightness still applies | `gbatek.txt:7980-7995` |
| The VRAM-display bank must be (or should be) LCDC. melonDS shows black otherwise | `gbatek.txt:8153-8155`; `melonDS/src/GPU_Soft.cpp:185-205` (`VRAMMap_LCDC & (1<<vrambank)`) |
| Mode 2 reads bank row L at line L. The read offset is ignored in mode 2 | `gbatek.txt:8157`; `GPU_Soft.cpp:190` |
| Capture source A taps engine A's layering output *before* the Select Video Input stage. So the engine keeps rendering, and capture still sees the game's picture, while display shows mode 2 or 3 | block diagram `gbatek.txt:8202-8235`; the "trail" example, `gbatek.txt:8180-8183`; melonDS renders `Rend2D_A` every line regardless of mode (`GPU_Soft.cpp:118`) |
| Capture source B = VRAM reads the **same** bank field, DISPCNT.18-19 | `gbatek.txt:8153`; `GPU_Soft.cpp:290-300` |
| Capture writes line L as it is produced, so a captured frame can only be shown the next frame | `gbatek.txt:8180-8183`; `GPU_Soft.cpp:118-127` (display, then capture) |
| Capture blend is D = (A·αA·EVA + B·αB·EVB)/16, clamped: additive, with no per-pixel replace | `gbatek.txt:8165-8172`; `GPU_Soft.cpp:341-380` |
| Engine B has no FIFO, no VRAM display, no capture and no 3D | `gbatek.txt:7982-7983, 8136, 8198` |
| POWCNT1.15: 0 = engine A to the lower screen, 1 = upper | `gbatek.txt:13664` |
| VRAMCNT is very probably readable (the in-game menu reads and restores it) | `hardware.md` section 0 item 3; test H4 |
| All VRAM takes 16/32-bit writes only | `gbatek.txt:7546-7550` |

## 2. New survey readings (line-0 samples, 45 titles, 53,400 frames each after boot)

**Which screen engine A drives** (share of frames with POWCNT1.15 = 1, i.e. engine A on top):
- 100% (A on top): Digimon, KSSU, Squeak Squad, NSMB, Yoshi, Spore, Platinum, SoulSilver, Nintendogs.
- 85-99%: Cory, DKR, Starfy, MK, Mario Party, 999, SPP.
- 0-3% (A on the bottom, touch screen): Cooking Mama, Ghost Trick, Kirby CC, MPP, Phoenix Wright, RE:DS, Rhythm Heaven, SM64DS, Tetris, Trauma Center 2, WarioWare, TWEWY, PvZ, Layton, Zelda PH/ST.
- **Toggling every frame (class E):**
  - Clubhouse, 100% of frames;
  - Pac'n Roll, 100%;
  - Peggle, 96%;
  - LEGO SW, 95%;
  - Golden Sun DD, 28%.

So a band on engine A lands on the action screen in about a third of titles, and on the touch screen in about 40%. In the touch-screen case the band can cover touch targets (Phoenix Wright's buttons, Tetris's hold). That is a functional cost to weigh beyond appearance.

**Capture (the "18 titles").**
- **Every frame:** Clubhouse, Pac'n Roll, Peggle and LEGO. Each alternates write block C/D at 256x192, source A only; Peggle uses source A = 3D-only.
- **Engine B in those four** alternates:
  - layer mask 0x04 (BG2 only: the captured bitmap as a sub BG);
  - layer mask 0x10 (OBJ only: the capture as bitmap OBJs).

  This is the standard dual-screen-3D scheme.
- **Golden Sun DD:**
  - captures on 36% of frames, 7% of them blended A+B from VRAM;
  - is itself in display mode 2 (bank D) on 45% of frames.
- **Pokemon Black:** A+B blends on 169 frames.
- **The rest capture only occasionally** (transitions, screenshots, menus): DKR 1220, MK 262, RE:DS 19, M&L 90, Sonic Colors 9, SRA 88, Nintendogs 33, Ghost Trick, SM64DS and NSMB under 5 frames each.
- No title uses the FIFO as source B.

**Games already using display mode 2** (bank in brackets), so they own at least one LCDC bank at those times:

| Title | Frames in mode 2 | Bank(s) |
|---|---|---|
| Golden Sun DD | 45% | D |
| 999 | 7.6% | A/B |
| Layton | 2.8% | A/B |
| Starfy | 1.3% | A/B |
| Sonic Colors | 0.5% | all |
| Trauma Center 2 | 0.3% | A/B |
| Pokemon Black | 0.4% | D |
| M&L BIS | 0.2% | C |

**The sub screen is almost never blank.** Strict definition: forced blank, mode 0, or DISPCNT_B bits 8-12 all clear.
- Stretches of 30 frames or more: Yoshi 40, Mario Party 13 and KSSU 5 in 15 minutes, all at transitions.
- Golden Sun 16% and Drawn to Life 9% are menu or editor screens.
- Everything else is 0-3%.
- In the class-E titles, OBJ-only frames have the OBJ layer on, so they don't count.

**Free engine-B BG layers.** Most titles leave one or two sub BG layers off. Examples (DISPCNT_B bits 8-12 on most frames):

| Title | Mask | BG layer free |
|---|---|---|
| MK | 0x1C | BG0/BG1 |
| NSMB | 0x1E | BG0 |
| KSSU | 0x17 | BG3 |
| PW | 0x1A | BG0, BG2 |

That makes engine-B layer *borrowing* possible in principle (section 5.4). It doesn't make it rule-clean.

## 3. Mechanisms, ranked

### 3.1 P2v: VRAM-display band from a free bank A-D. Rank 1 where a bank exists

**Pixels.** Bank X (A-D) is LCDC, or disabled and set to LCDC by us. Its rows [y0, y0+N) hold the band image: 512 bytes per row, 16 KB for N = 32. This is written once per notice change, by the CPU or by our NDMA from module RAM, in idle.
- **Entry IRQ at the end of line y0-1:**
  - read DISPCNT A byte 2 (`b`);
  - if the mode bits are 1, `STRB [0x04000002], (b & 0xF0) | (X<<2) | 2`. Bits 20-23 (OBJ boundary, H-Blank free) are kept.
- **Exit IRQ at the end of line y0+N-1:** if byte 2 still reads `(X<<2)|2` in its low nibble, `STRB` the saved low nibble back.
- **Timing:** NDMA H-Blank counters, as in P2b (`display-library.md` section 11.4), through the patched `OS_IrqHandler` (R4).
- **Dispatcher order:** NitroSDK's V1/V2 dispatcher picks the highest pending IE&IF bit (`clz`, `survey.md` section 5). So our NDMA bits 28-31 are served before a coincident game H-Blank (bit 1) or VBlank (bit 0) IRQ. The STRB should therefore be the first thing the patched dispatcher does for our bit, about 20-40 cycles, with no C call.

**What the game sees:**
- DISPCNT A byte 2 low nibble = mode 2 / bank X for N lines (an R1-like residual).
- If bank X was disabled, VRAMCNT_X reads 0x80 instead of 0x00 while a notice is up. Restored at the end; a game write to it wins at once.
- Writes to 16 KB of a bank the game isn't using. Saved and restored when the bank isn't pristine (section 3.1.2).
- Nothing else: no FIFO, no main-RAM traffic, no change to VCOUNT, H-Blank, timers, OAM or palettes.

**Cost in the game's working time.**
- Per shown frame: 2 IRQ entries (BIOS vector, dispatcher test, STRB, ack), plus 2 single-word NDMA fills per H-Blank to NDMA's own FDATA (I/O, not main RAM; to confirm, H11).
- Estimate 150-400 ARM9 cycles per frame, about 0.01-0.04% of a 1.12M-cycle frame. **The display's reads of bank X don't touch the ARM9/ARM7 main bus.** Each VRAM bank has its own port (`gbatek.txt:7848-7852`, VRAM waitstates), and the game doesn't access bank X.
- The bound X_v is measured on hardware (S3). Because X_v doesn't scale with N, a 32-line band costs the same as an 8-line one.

**Why it needn't be idle-gated.** P2b was confined to halted time for two reasons:
- the FIFO's bus traffic;
- any game IRQ during the band had to abort it (11.1 step 4).

P2v has neither. Still prefer idle-gated placement where the game idles, because then the R1 exposure meets only IRQ handlers, not game code.

**Placement (per game, from the idle census and S4):**
- **Bottom band, ending at line 191.**
  - A game read-modify-write that spans the exit can only re-set mode 2 *after* line 191, i.e. in VBlank, where nothing is displayed. A late-VBlank check (below) clears it before line 0.
  - There is no visible "spill" at all, unlike P2 top, whose spill showed as a thicker border.
- **Top band (lines 0..N-1), for class-A titles busy at the bottom.**
  - Needs the NDMA delay timer for a line-261 entry (H2).
  - The R1/R2 exposure here meets game code. Mitigations:
    - the `GX_DispOff` patch (R3);
    - the guard IRQ at N+M-1;
    - fill bank rows N..N+M-1 with the band's border colour, so a spill shows as a thicker border for one frame.
- **The leak fixer is stronger than P2's:**
  - A DISPCNT A value with mode 2 and **bank X** can only be ours: the game never selects a bank it doesn't use.
  - So the late-VBlank check (NDMA delay timer at about line 255) and the module's idle pass may reset it to mode 1 unconditionally, with no `armed_this_frame` ambiguity.
  - The remaining visible failure is a game `GX_DispOn` mid-frame restoring a leaked mode 2. That shows bank X (band plus black) for the rest of one frame. Rare; the `GX_DispOff` patch removes the SDK path.

**Stand-down guards:**
- DISPCNT A mode = 1;
- **DISPCAPCNT.31 = 1 with capture source ≠ 0 (source B or blend):** our bits 18-19 would feed bank X into the game's capture on band lines (Golden Sun DD, Pokemon Black);
- the game's capture write block (DISPCAPCNT.16-17) ≠ X;
- VRAMCNT_X still LCDC/ours;
- MASTER_BRIGHT A factor < 12.

Capture source A alone is unaffected (it taps layering before the selector). So **the class-E capturers and the occasional capturers are compatible** whenever they have a free bank.

#### 3.1.1 Which classes and games
| Class | P2v? | Why |
|---|---|---|
| A busy at bottom (MK, Cory, DKR, 999, Layton) | yes, if bank | top band, or bottom band on busy frames: cost X_v only |
| B raster IRQs (KSSU, Pokemon B, AC, Starfy, M&L, Trauma 2, Clubhouse) | yes, if bank | game IRQs no longer abort it. Their raster writes on band lines have no visible effect (the band replaces the lines). Our dispatcher work delays a coincident H-Blank handler by about 20-40 cycles, against a 17.7 µs H-Blank: S5 checks the line just below the band. Bottom band ending at 191 has no game raster line below it |
| C BIOS waits (Sonic Rush, RE:DS, SPP, MPP) | yes, if bank | timing from NDMA; no idle signal needed. Needs only the dispatcher patch (the handlers are V1) |
| D never halts (Golden Sun DD, Code Lyoko) | yes, if bank | as C. Golden Sun's V4 handler needs a 3rd signature or the BIOS-vector wrapper |
| E swap every frame | no alone | flicker; see 3.2 |
| F DSi-enhanced in DS mode | yes, if bank | the same hardware path |

The coverage limit is entirely the bank.
- **Prior expectation:** a minority of titles have a free A-D bank.
  - Typical 2D layouts use A = main BG, B = main OBJ, C = sub BG and D = sub OBJ.
  - 3D titles put textures in two or more banks.
  - Some games use mode 2 or capture themselves (section 2).
- S1 measures it. If S1 finds a free bank in under about 25% of titles, P2v is a specialised tool, not the backbone.

#### 3.1.2 "Free" defined, and the save/restore
A bank qualifies if all of these hold:
1. VRAMCNT_X (readback, H4) is disabled or MST 0;
2. NitroSDK's bank bookkeeping in game RAM (read-only; `GX_SetBankFor*` keeps the free/LCDC set) agrees;
3. the per-title census (S1) saw no CPU, DMA or capture write to the 16 KB slice we use for 99% or more of play frames.

At notice start, copy the slice out (16 KB into module RAM, in idle). At the end, restore it if the slice still equals our image; if it doesn't, the game wrote it, so drop the bank for this title. The game reading a disabled bank's parked data mid-notice would see ours. That is the Ketsui risk (`hardware.md` 2.4); S1's write-trace gate makes it a per-title allow-list matter.

### 3.2 E-stamp: notices for screen-swapping dual-3D games (class E). Required for E

**Why only this works.**
- In class E each physical screen S shows:
  - engine A live on alternate frames;
  - engine B on the other frames, showing the bank captured on S's last live frame.
- A band on engine A alone shows on S every other frame: 30 Hz flicker.
- Engine B's frames show the capture, which taps engine A *before* our band (source A = layering). So the band never reaches the capture.
- The only ways to put a notice in engine B's frame are its layers (game state) or the captured pixels (game VRAM).

**Mechanism.** Per frame, for the chosen screen S:
- **Frames where engine A drives S** (POWCNT1.15 read at entry): band on engine A. P2v if a bank is free; class E usually has none (A/B textures, C/D captures), so P2b FIFO in idle.
- **Frames where engine B drives S:**
  - stamp the same band rows into the bank the game captured on the previous frame;
  - when: after capture ends at line 192 (DISPCAPCNT.31 auto-clears; `gbatek.txt:8160-8162`) and before engine B scans line y0;
  - layout: both the BG2 direct-bitmap and the 2D-bitmap-OBJ views read the bank as a linear 256-wide image, the same layout capture wrote, so the stamp is a row copy at the capture write offset;
  - addressing: write through whichever mapping VRAMCNT readback shows (LCDC `0x068x0000`, sub BG `0x06200000` or sub OBJ `0x06600000`);
  - set bit 15 (alpha) on our pixels: bitmap BG/OBJ pixels with alpha 0 are transparent.
- **Content:** identical pixels on both kinds of frame, so the band is steady.

**Game state touched:** 16 KB per frame (N = 32) of the game's capture bank, in rows the game itself overwrites by capture on its next frame for that screen. Gate on:
- capture source = A only;
- the bank used afterwards only as an engine-B display source (VRAMCNT MST 4 / sub, no texture use);
- no A+B blend reading it back;
- no screenshot or photo feature (per title).

**Cost:** 4096 word writes to VRAM, about 8-16k ARM9 cycles (about 1% of a frame), plus the band's own cost on the A frames. Both must fall in idle time, since there is no bus-free way to stamp. This needs the idle census for class E. The stamp source can sit in module RAM (main bus) or, better, in a second VRAM bank if one were free (rare).

**Visual result:** a steady band on screen S. Pixels outside the band rows are exactly the game's. One subtle point: engine A's band gets master brightness A and the stamped frame gets master brightness B. Class-E games normally keep both equal; gate on equality.

**Residual (owner ruling needed): "E1".** DScheevos writes into a game VRAM bank between the game's capture and its display of that capture. Nothing the game later reads is affected under the gates above.

### 3.3 P2b FIFO band (existing): the fallback where no bank is free
Unchanged. Its bus cost still forces idle-gating, so it serves only frames where the game halts before the band. P2v and P2b share the timing code, the dispatcher patch and the guards. A presenter can pick per frame: P2v if a bank is free, else P2b if idle, else nothing.

### 3.4 Engine B (the other screen): nothing rule-clean, and P4 serves nobody
Every engine-B pixel needs:
- a BG layer or OBJ, with its control register (BGxCNT_B / OAM);
- VRAM mapped to engine B (C/H/I for BG, D/I for OBJ);
- palette entries, unless it is a direct-colour bitmap BG, which needs an Extended BG slot in the game's BG mode;
- and the layer must win priority: BG0 beats other BGs of equal priority (`gbatek.txt:926`); OBJ beats a BG of equal priority.

Window registers (WININ/WINOUT, readable) can mask our layer out. BLDCNT/BLDY (fades) apply to it.

- **P4 as designed (backdrop-only sub screen):** its trigger doesn't happen in play (section 2). **It serves 0 of the 27 titles that reached play.** Drop it as a coverage tool; keep it only for transitions.
- **Generalised P4: borrow a free sub BG layer + bank H/I + 16 palette entries over a static sub screen:**
  - possible in titles with a free layer and no C mapping at `0x06200000` (C and H overlap there);
  - residuals R12-R14 widen to "a game-visible layer appears over the game's sub-screen content".
  - **Opaque occlusion of the least-used screen is the only engine-B upside:** the sub screen in many titles is a HUD or static art (KSSU's status panel, MK's map and standings, see screenshots), so covering 32 rows of it hides less than covering the action screen. The data shows that screen changes rarely, not never.
  - Rank below P2v/P2b. Allow list plus per-title traces (S6).
- **OBJ 127 + a free OBJ VRAM slice:** Bakkerrrs' hardware evidence (garbage when parked tiles were reused, `hardware.md` 2.4). No.

### 3.5 Display-capture compositing: rejected
The idea: capture engine A's output blended with the notice (source B = FIFO or VRAM) into free bank X, and show X by mode 2 for the whole frame.
1. **Latency and judder.** A frame can only be shown the frame after it is captured (section 1).
   - On entry, the first composited frame repeats the frame already shown live: a visible hitch, like a lag frame to the eye.
   - On exit, one game frame is never shown: a skip.
   - In between, the engine-A screen runs 16.7 ms behind engine B, audio and input. When engine A drives the touch screen, the touch point is a frame off from the picture.
   - This breaks "nil lag" in spirit and "no visual compromise" in letter.
2. **Additive only** (`gbatek.txt:8165-8172`): where the notice's α = 1, D = A·EVA/16 + B·EVB/16. EVA is global.
   - With EVA = 16 the game shows unchanged elsewhere, but the notice can only *add* light: white text vanishes on bright scenes, and no dark backing box is possible.
   - With EVA < 16 the whole picture dims.
3. **It needs a free bank anyway,** and given one, P2v is strictly better: same frame, opaque, no latency.
4. **With source B = FIFO** it costs about 9-10% of main bus for all 192 lines every frame. That can't be confined to idle time, since capture spans the whole frame.
5. **It fights the 18 capturing titles:** one capture unit, and the game rewrites DISPCAPCNT each frame.

There is one non-rejected use of capture: the class-E stamp (3.2), which piggybacks on the game's own capture and adds no latency.

### 3.6 3D engine and rear plane: rejected
- 3D output reaches the screen only as engine A's BG0 (DISPCNT bits 3 and 8, the game's) or through capture (3.5).
- In 2D titles, BG0 is a text layer in use: KSSU, Tetris, Digimon, MPP, Yoshi, TWEWY, Spore and Platinum all have DISPCNT.3 = 0.
- The rear-plane bitmap needs banks in texture slots 2/3 (MST 3), and CLEAR_COLOR and CLRIMAGE_OFFSET are game-owned.
- In 3D titles every geometry register is the game's (matrix stack, FIFO, SWAP_BUFFERS).
- There is no route that touches no game state, and every route is worse than P2v.

### 3.7 Borrowing a game bank to LCDC on band lines: rejected
The idea: engine A's output on band lines is hidden anyway, so remap one of the game's engine-A BG/OBJ banks to LCDC for those lines.
- Rows y0..y0+N of that bank are the game's tile/map data, so showing a notice there means overwriting it. That is damage.
- OBJ lines are prepared one line ahead, so restoring the mapping late clips sprites.
- Texture banks are read ahead by the 3D renderer.

Dead.

## 4. Classes this angle can't close alone

- **Games with no free bank AND no idle window before VBlank** (some of A and D).
  - P2b would cost main bus while the game works (about 3% of bus on band lines). That is a lag risk the owner already rejected.
  - Under this angle they get the LEDs plus the menu only.
  - Evidence needed to declare them unserved: S1 (no free slice) intersected with the idle census (no halt before line 192-N on 95% or more of frames). The list is unknown until both run.
- **Class E with no idle time for the stamp and the band.** Same evidence: S1 plus the idle census on Clubhouse, Peggle, Pac'n Roll and LEGO SW.

So 100% on screen is **not** provable from this angle. The combination below is the best coverage this angle offers, and the remainder is measurable.

**Proposed combination:**
- engine-A band: P2v where a bank is free, at any time; else P2b idle-gated;
- E-stamp for class E;
- P4-generalised (sub-screen layer) for allow-listed titles where neither works and the sub screen is static;
- the camera LED for every challenge indicator, in every title.

## 5. Occlusion: is covering game pixels a compromise?

- **RetroArch.** Its achievement popup is drawn *over* the game picture:
  - a badge square plus a text panel on a semi-opaque backdrop;
  - height 4 font lines;
  - anchor configurable (top/bottom, left/centre/right);
  - visible for `CHEEVO_NOTIFICATION_DURATION 4000` ms plus slide animations (`gfx/widgets/gfx_widget_achievement_popup.c`).

  So the RA reference client hides game pixels for about 4-5 s per unlock, and RA accepts that in hardcore.
- **Proposed ruling:** a notice may hide game pixels on the band's lines for bounded, short periods. That isn't "damage", since nothing the game draws in memory or on later frames changes. Damage, garbage, tearing and flicker stay forbidden.
- **Footprint.**
  - Engine-A mechanisms can't do a corner box. The band is full-width; a 16-line compact band is 8.3% of one screen.
  - Hence:
    - unlock: 32 lines, 3 s or less;
    - progress: 16 lines, 2 s or less, rate-limited;
    - challenge: the camera LED steady, plus a 16-line band of 1 s or less at prime and unprime;
    - no persistent on-screen tracker unless the owner opts in per title.
  - Bottom placement avoids the top-of-screen HUD most DS games use (KSSU's top screen has no HUD; MK's lap counter is top-right; PW/Layton keep buttons at the bottom of the touch screen, so for those a top band is better: per-title placement).

## 6. Experiments (pass marks set now)

- **S1. Bank census (melonDS, survey runner extension).** Per frame, log:
  - VRAMCNT A-I;
  - NitroSDK's bank state;
  - CPU/DMA/capture writes per 16 KB slice of A-D.

  **Pass for P2v as backbone:** at least 50% of the titles that reached play have a bank A-D (disabled or LCDC) with a 16 KB slice at rows 160-191 or 0-31 unwritten on 99% or more of play frames. Under 25% means P2v is a niche presenter.
- **S2. Mode-2 mid-frame band (DSi display-lab).** STRB DISPCNT byte 2 to mode 2 / bank X at the end of line 159, back at 191, from NDMA H-Blank IRQs; plus a top variant (entry at line 261 via the BCNT delay). Also try bank X disabled vs LCDC vs MST 1. **Pass:**
  - 600 photographed frames with clean band edges (no torn line);
  - bytes 0, 1 and 3 read back unchanged;
  - the bank requirement recorded (LCDC needed or not).
- **S3. Bus independence and X_v (DSi).** ARM9 main-RAM busy loop and ARM7 timer probe, band on vs off, 10,000 frames. **Pass:**
  - ARM9 and ARM7 loop rates differ by no more than the IRQ count times the measured entry cost;
  - T1's slack probe gives X_v ≤ 500 cycles.
- **S4. Capture under mode 2 (DSi).** Capture source A at 256x192 while the band shows. **Pass:** the capture is bit-identical to a band-off run, i.e. capture taps before the selector on hardware too.
- **S5. Raster neighbour (melonDS --thumb-cycle, then DSi).** KSSU, Pokemon Black and Animal Crossing with a forced P2v band every 5 s for 10 minutes. **Pass:**
  - zero pixel differences outside band rows versus a notice-off run with the same inputs;
  - identical lag count;
  - DISPCNT A never mode 2 / bank X at line 0.
- **S6. E-stamp (melonDS, then DSi).** Peggle, Clubhouse, Pac'n Roll and LEGO SW, forced notice on each screen. **Pass:**
  - the band is present on the chosen screen on 100% of frames while the notice is up (no 30 Hz gap);
  - zero pixel differences outside the band rows;
  - identical lag count;
  - the game's next capture of that bank is bit-identical to a stamp-off run (we leave nothing behind).
- **S7. Band-line DISPCNT exposure (survey re-run).** Byte-2 reads on lines 160-191 followed within 2 lines by a write at 192-195, per title; the R1 hazard for the bottom band on busy frames. **Pass:** zero sites in a title, or the `GX_DispOff` patch covers them.

## 7. What the owner rules on (new residuals)

| # | Residual | Size |
|---|---|---|
| V1 | DISPCNT A byte 2 low nibble = mode 2 + bank X on N lines | as R1; bottom band: spill only into VBlank |
| V2 | VRAMCNT_X disabled → LCDC while a notice is up (only if the bank was disabled) | one byte, restored; a game write wins |
| V3 | 16 KB written into an unused bank slice, saved/restored | S1-gated per title |
| E1 | Band rows stamped into the game's capture bank between capture and display (class E) | 16 KB/frame of VRAM the game rewrites itself next frame |
| O1 | Opaque full-width band hides game pixels for 3 s or less | RetroArch-equivalent occlusion |
