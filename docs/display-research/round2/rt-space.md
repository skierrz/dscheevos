# Red-team: prop-space.md (P2v VRAM-display band, E-stamp, engine B, occlusion)

Read-only. Sources: GBATEK (`(local scratch)`, "G:line"), melonDS (`tools/melonDS/src`), nds-bootstrap (`external/nds-bootstrap/retail`), display docs (`~/Developer/DScheevos/dscheevos-display/docs`), the M1 survey frame logs (`scratchpad/survey/runs3*/`) and the idle census (`~/Developer/DScheevos/private/census/out/*/report.txt`). The only new numbers are a re-read of the survey logs (DISPCNT A mode/bank and DISPCAPCNT per frame, script in `scratchpad/rtspace/`, run inline) and the census report headers. Speculation is marked **[spec]**.

## Verdict

- **P2v is not new.** It is `hardware.md` section 2.2, whose verdict was "fallback only... writes game VRAM. Allow-list only".
- The proposal adds one idea: run it on busy frames because "nothing is shared". That claim is where it breaks.
  - VRAM access isn't shown to be private.
  - The cost bound is several times too low.
  - Leaving the idle gate also loses the edge-timing and game-code protections the gate gave for free.
- The "free bank" stays an inference about the future. The survey already shows games that pick each of A-D for their own mode 2.
- E-stamp is a sound idea in outline. As specified, it lands its 8-16k-cycle stamp in the game's working time, and it can leave the band behind in a frozen image.
- Engine-B and capture-compositing rejections mostly hold.
- "Occlusion is not damage" is a reframing the owner has to rule on, not a finding.

## Blockers

### B1. "Free bank" means free now, not provably free; a wrong guess damages the game and the leak fixer makes it worse

**Evidence**
- **VRAMCNT A-I are marked write-only** (G:7475-7483, "(W)").
- Readback is inferred from the in-game menu (`hardware.md` §0.3) and is untested on hardware (H4).
- Even if readable, it gives the current mapping, never intent. Nothing traps a later game write to VRAMCNT_X.
- **Games pick these banks for their own mode 2.** Re-read of the survey logs (DISPCNT A at line 0, frames 600+):
  - Sonic Colors uses mode 2 from bank **A (123 frames), B (127), C (5) and D (4)**: every candidate bank.
  - Golden Sun DD uses mode 2 from bank D on 24,150 of 53,400 frames.
  - The survey's own table lists 999, Layton, Starfy and Trauma Center 2 on A/B at transitions.
  - The surveyed segment is 15 scripted minutes, mostly the first stage (`survey.md` §2, "Coverage of a game's modes is partial"). Later levels aren't covered.
- **"Disabled or MST 0" counts LCDC as free. In NitroSDK, LCDC is an explicit allocation.**
  - A game maps a bank to LCDC to write it by CPU: capture targets, mode-2 images, `GX_BeginLoadTex` staging.
  - So an LCDC bank is the state *most* likely to be in use.
  - **[spec]** on how often this happens; the SDK pattern is standard.
- **The Ketsui lesson.** "Disabled entry means free" failed on hardware because parked banks were reused at stage transitions (`hardware.md:153`, `prior-art.md:28`).

**What a wrong guess does**

| Case | Result |
|---|---|
| a. The game maps X to tex/BG/OBJ mid-notice | It reads our 16 KB image instead of its parked data: corrupted textures or tiles in the game's own picture until the restore. That is damage to what the game draws (hard rule). The compare-and-restore (§3.1.2) repairs memory afterwards, not the frames already shown |
| b. The game selects X for its own mode 2 or capture | §3.1 claims "mode 2 + bank X can only be ours" and lets the late-VBlank check and idle pass **reset it to mode 1 unconditionally** (prop-space.md:148-149). For Sonic Colors or Golden Sun that kills the game's own VRAM-display effect. If the game sets DISPCNT once per scene and doesn't rewrite it, the effect stays dead until its next write. Golden Sun already does an RMW mode 1→2 at line 9 (`survey.md` §4.2), so the collision isn't hypothetical |
| c. The game captures less than 256x192 into X, or writes only part of X | Its mode-2 picture shows our band rows inside its own image |
| d. The game changes VRAMCNT_X during the band | melonDS shows black (`GPU_Soft.cpp:187-205`). Hardware is unknown (G:8154, "can be (or must be ?)"). Either way: one frame of black or garbage rows |
| e. The restore after the game remapped X | Writes through the LCDC address go nowhere, so the game's parked data is lost for good |

**Fix**
- Drop the unconditional reset. Reset only when all of these hold:
  - our armed flag;
  - VRAMCNT_X is the value we left;
  - the game's SDK bank bookkeeping still shows X unallocated.
- Accept **disabled** banks only, never MST 0.
- Gate per title on a **static whole-ROM scan (overlays included)** for `GX_SetBankFor*`, `GX_SetGraphicsMode`/DISPCNT writes and DISPCAPCNT write-block constants that can name X, not on a 15-minute trace.
- Treat P2v as allow-list only, as `hardware.md` 2.2 concluded.

**Test (S1+)**
- Run S1 on long runs that reach late game (save states at later stages).
- Pass: zero VRAMCNT_X writes and zero DISPCNT bank-X selections by the game in every state of the title.

### B2. "No need for the game to be halted" loses three protections the idle gate gave, not just the bus one

Section 11.1 put the strip in halted time for lag and for safety. P2v keeps the hazards that have nothing to do with the FIFO.

**a. Lag.**
- On busy frames the IRQ cost (B3, below) lands in the game's working time.
- Class A is defined as "still working at 156-189". MKDS has no OS_Halt in 24% of census intervals; Golden Sun 99.9% and Code Lyoko 99.8% (`census/out/{mkds,goldensun,code-lyoko}/report.txt`).
- Adding even about 2k cycles to a frame that finishes near VBlank can create a lag frame. That is the R7 "rare lag frame" the owner already rejected (`display-library.md` §11 item 2).

**b. Edge timing.**
- The STRB must land inside the H-Blank of line y0-1: about 1,190 ARM9 cycles (99 dots x 6 x 2; G:7832, 7860).
- On busy frames the game may be in a CPSR-I or IME=0 critical section, or in its own IRQ handler. NitroSDK handlers don't nest.
- nds-bootstrap itself holds IME=0 across SD I/O on some paths (`cardenginei/arm9/source/cardengine.c:1473-1477`, 1501-1505, the DSiWare save paths).
- Late entry gives a torn first line and a band height that varies from frame to frame (flicker).
- A late or lost exit on a top band spills mode 2 over game lines. A lost exit at 191 carries into the next frame until the late-VBlank IRQ, which is blocked by the same section.
- melonDS can't show any of this: it latches per line and its IRQ latency is idealised (`survey.md` §2). S2 runs in a lab ROM with IRQs always enabled. **[spec]** on how often this happens in real games.

**c. Game code writes DISPCNT during the band.**
- Halted time exposed only IRQ handlers. Busy time exposes game code.
- Survey (window lines 0-35 only):
  - Mario Party DS: 114 absolute writes at lines 17-19;
  - Super Princess Peach: 66 absolute writes at lines 6-30;
  - Golden Sun: RMW to mode 2 at line 9.
- Not surveyed at 160-191 at all. Per-frame write counts suggest exposure there: TWEWY 9/frame, WarioWare 5, Animal Crossing 3.6.
- An absolute write truncates the band (our notice flickers or tears). An RMW that keeps bits 18-19 = X turns the rest of the frame into bank X (garbage).
- "Bottom band ending at 191" only protects against RMW spill, not truncation.

**Fix**
- Keep idle-gating as the default.
- On busy frames, allow P2v only with all of these:
  - (1) a slack gate whose margin is well above the measured X_v (owner ruling, since R7 was rejected);
  - (2) a self-check in the entry and exit handlers: read VCOUNT and DISPSTAT.1 at the STRB. If not still H-Blank of the right line, don't enter, and end the whole notice early rather than show a partial band;
  - (3) S7 extended to lines 150-195, counting **absolute** writes as well as RMW.

**Test**
- On DSi, log the late-STRB count per title (from the self-check) over 10 minutes per class-A/D title.
- Pass: 0 late edges on frames where the band was shown.
- In melonDS, add a per-line CPSR.I/IME sample to the survey to find critical sections at y0-1 and y0+N-1.

## Major

### M1. "The display's reads of bank X don't touch the bus; each bank has its own port" is unsupported, and its citation is wrong

**Evidence**
- The proposal cites G:7848-7852. In this copy those lines are DISPSTAT/VCOUNT text.
- GBATEK's two VRAM-waitstate passages say the opposite of "free":
  - G:7790-7794: "on NDS9, a one cycle wait can be added to VRAM accesses (when the video controller simultaneously accesses it) (that can be disabled by Forced Blank)... additional VRAM waitstates occur when using the video capture function";
  - G:7863-7866: "The display controller performs VRAM-reads once every 6 clock cycles, a 1 cycle waitstate is generated if the CPU simultaneously accesses VRAM. With capture enabled, additionally VRAM-writes... total VRAM-read/write access rate is then once every 3 cycles."
- One read per 6 cycles is exactly one pixel per dot, i.e. the mode-2/capture-B display stream. That stream is what P2v adds; the BG/OBJ engine keeps rendering underneath (`GPU_Soft.cpp:118`).
- Nothing in GBATEK says the wait is per-bank. melonDS doesn't model it at all.

**Impact.** **[spec]** If the wait is not per-bank, game CPU or DMA VRAM traffic on band lines is slowed by up to 1 bus cycle in 6. Worst case, a game hammering VRAM on all 32 band lines loses about 8k bus cycles (about 16k ARM9 cycles, 1.4% of a frame). In practice it is far less, but it isn't 0, and it isn't in X_v's estimate.

**Test.** Add to S3, on DSi:
- a CPU loop of 16-bit VRAM read/writes to a game-mapped bank ≠ X, and one to the same bank;
- band on vs off vs forced blank (the control, per G:7792);
- 10,000 frames.

Pass: the loop rate differs by no more than noise, and only the same-bank case differs.

### M2. The 150-400-cycle bound is several times too low by the project's own measurements

**What the bound leaves out:**
- **IRQ entry.** The project measured BIOS vector → dispatcher → handler → return at **300-900 cycles each, 750-880 for the IPC hook** (`hardware.md` §2.1 "IRQs"; `display-library.md` R8). The proposal uses 20-40 cycles: the STRB alone, not the entry.
- **IRQ count.** At least 3 per shown frame (entry, exit, late-VBlank check), 4 with a top-band guard. The proposal counts 2.
- **Per-frame arming on busy frames.** It can't arm from idle, so it arms from the VBlank hook: 250-400 cycles (`display-library.md` §11.4 B3).
- **The H-Blank counter's NDMA writes.** These now run during working time (H11 unconfirmed).
- **The dispatcher `tst`** on every game IRQ.
- **Cache pollution** from the module's handler code and data evicting game lines **[spec]**.
- **M1's VRAM waits.**

**Realistic range:** about 1.2-4k cycles per shown frame, about 0.1-0.35%.

That is still small, but every word in "150-400 in working time" needs replacing with a T1 measurement on a class-A stand-in. Fix: restate X_v as unknown, bounded only by T1/S3.

### M3. The E-stamp's timing falls in working time, and its two halves have to succeed together

**Timing.**
- The stamp goes "after capture ends at line 192 and before engine B scans y0".
- For a bottom band that window is VBlank plus the start of the next frame, when the game's frame work runs. 4,096 VRAM word writes (8-16k cycles, about 1%) would land in working time.
- Idle is at the end of a frame, before the capture of the band rows has finished.

**Fix (it works):**
- Race the capture beam. Stamp row r right after capture writes it (lines y0..191), while the game is halted.
- The bank is LCDC throughout capture (G:8146), so there is also no race with the game's remap of the bank to sub BG/OBJ in VBlank, a race the proposal doesn't address.

**All-or-nothing.**
- The A-frames need P2b in idle (class E has no free bank, by the proposal's own reading). The B-frames need the stamp in idle.
- If either misses, the band shows at 30 Hz: flicker, a hard-rule violation.
- Census, intervals with OS_Halt:

  | Title | With OS_Halt |
  |---|---|
  | Clubhouse | 80% |
  | Peggle | 75% |
  | LEGO SW | 57% |
  | Pac'n Roll | 0%: it waits in BIOS (class C), so it has no idle hook yet |

- Fix: start a class-E notice only after N consecutive frames with both halves possible, and end it (not skip a frame) the first time either half misses.

**Test.** Add to S6:
- the late-stamp count;
- the per-frame both-halves-present rate. Pass: 100% while the notice is up.

### M4. The E-stamp can leave our band inside the game's image

**Problem.**
- "The game overwrites the bank next frame" holds only while the game keeps capturing.
- Dual-screen-3D games commonly freeze the last captured frame on one screen for pauses, menus and transitions **[spec, typical pattern]**.
- If the notice's last stamp precedes such a freeze, engine B keeps showing our band in the game's still image after the notice ends. That is game VRAM visibly altered beyond our window.

**Fix.**
- Before stamping, save the captured rows (16 KB per frame; can be done during the stamp itself as a swap).
- When the notice ends, or the game doesn't re-arm DISPCAPCNT for that bank, restore them.
- Also gate on engine B showing the bank 1:1:
  - BG2PA-PD/BG2X/Y identity, or the OBJ grid at known positions;
  - for LEGO, the C captures are 3D-only (survey: block C, source A = 3D), so engine-B layers composite over them.

**Test.** S6: end a notice, then pause the game within 1 frame. Pass: no band pixels in any later frame.

### M5. Coverage claims overstated

- **Golden Sun DD (class D: "yes, if bank").** Re-read of its logs:
  - mode 2 from bank D on 45% of frames (stand-down: mode ≠ 1);
  - capture into D on 36%, A+B blended from VRAM on 4,238 frames (stand-down);
  - screen swap on 28% (class-E flicker without the stamp, and its A+B blend excludes the stamp).
  - Realistically it is served on a minority of frames, intermittently.
- **Sonic Colors.** All four banks used for its own mode 2: no bank is "free" by any trace-based definition.
- **Pac'n Roll (class E).** A BIOS waiter, so E-stamp and P2b depend on idle-coverage's unbuilt hook.
- **Class F ("same hardware path").** F's defining issue is memory. P2v adds a 16 KB save buffer per notice (32 KB double-buffered) in module RAM.
- **Placement.**
  - Engine A drives the touch screen in about 40% of titles (prop §2). P2v can't honour the owner's placement setting there (`display-library.md` §11, item 1 is a hard owner requirement).
  - It also covers touch targets (Phoenix Wright, Layton, Tetris), which is a gameplay cost, not just a visual one.
  - The engine-B rejection leaves that requirement unserved, and the proposal doesn't say so.
- **The summary** (prop §0 item 1) presents classes A-D as served. The body's own prior is "a minority of titles have a free bank".
  - Fix: lead with "P2v serves A-D only where S1 finds a bank, expected a minority".

### M6. Occlusion: the owner has to rule; it can't be argued as "not damage"

- The bar is "zero visual compromises... nothing the game draws damaged". An opaque full-width band hides 8-17% of the action screen (or touch screen) for up to 3 s. That is at least a visual compromise in the letter.
- The current design already lists it as residual R9.
- The RetroArch comparison is weak:
  - RetroArch's popup is a partial-width, semi-opaque box on a large display;
  - engine A can't do either of those things (modes 2 and 3 replace whole lines, G:7988-7993).
- The proposal also drops the on-screen **challenge indicator "while active"** (a brief requirement) for the camera LED plus a 1-s band at prime and unprime. That is a scope reduction to declare, not a footprint choice.
- Fix: present O1 as a ruling with these numbers, and list the challenge indicator as LED-only under this design.

## Minor

- **m1. Mis-citations.**
  - VRAM waitstates are at G:7788-7794 and G:7863-7866, not 7848-7852 (that is DISPSTAT/VCOUNT).
  - BG0 priority is at G:925, close enough.
- **m2. nds-bootstrap also moves A-D.**
  - The Transformers fix remaps A-D to texture slots whenever `VRAM_CR == 0x80808080` (`cardenginei/arm9/source/cardengine.c:448-462`). Our V2 (disabled → LCDC) can make that condition true.
  - The IGM screenshot picks a capture bank by reading VRAMCNT A-D and captures into it (`cardenginei/arm9_igm/source/inGameMenu.c:283-299`), overwriting our slice.
  - Fix: stand down for tFormersFix titles; re-render the slice after the IGM closes.
- **m3. Top band, line N.**
  - **[spec]** If hardware engine A skips line-ahead OBJ preparation while in mode 2 with capture off, the first game line after the band loses sprites.
  - melonDS always renders (`GPU_Soft.cpp:118`), so it can't show this.
  - Test: S2 checks sprite integrity on line N.
- **m4. Exit at 191 contradicts `display-library.md` §11.4 step 3** ("exit at 191-K, K ≥ 2"), with no reason given.
- **m5. P4's "0 of 27"** is a register-level, line-0 count. "Layers on but transparent or blank" sub screens are invisible to it. Also, `an2.py` doesn't count a run still open at the end of the file. The conclusion probably holds; the wording should say "register-blank".
- **m6. Mode 2 from a non-LCDC bank is undefined on hardware** (G:8154). S2 already plans the test; record the result as a guard, not a note.
- **m7. Accepted.**
  - Capture-compositing rejection: additive blend, G:8165-8172; +1 frame latency, G:8180-8183.
  - The 3D/rear-plane rejection.
  - Bank-borrow-on-band-lines rejection.
  - Mode 2 reads row L, ignoring the read offset (G:8155; `GPU_Soft.cpp:190`).
  - The capture source-B coupling via DISPCNT.18-19 (`GPU_Soft.cpp:290-300`). The proposal's stand-down guard (capture enabled with source ≠ A) correctly covers it, given capture starts only at the next line 0 (G:8160).

## What would change the verdict

1. **S1+ (B1).** A whole-ROM static scan and late-game states find a disabled bank that is never named by the game in at least 50% of titles.
2. **M1 test.** Shows per-bank VRAM arbitration.
3. **T1/S3.** Puts X_v below the per-title slack margin.
4. **The late-edge self-check (B2).** Shows 0 late edges on busy frames.

With all four, P2v becomes a real busy-frame presenter for allow-listed titles. Without them it stays what `hardware.md` 2.2 said: an idle-gated, allow-listed alternative to P2b's FIFO that saves bus time.
