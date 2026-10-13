# Showing RA notices during play on a DSi

Research of 2026-10-11 (three research angles plus a critic pass). Since built: the pre-boot placard (option 1) and the LED cues (option 3), each with the changes noted under it.

## Question

How can DScheevos show RetroAchievements notices *during play* on a DSi running DS games natively through nds-bootstrap? The notices are unlock, challenge indicator, measured progress, leaderboard start/tracker/result, and the hardcore mode at game start.

## Rules every option is judged against

1. No pausing.
2. No slowdown and no added lag frames.
3. No corruption or visual glitch beyond the notice itself.
4. Never touch the game's RAM or state.
5. No way to change game state, and nothing shown beyond what RA allows (only rc_client event data).
6. The game's wireless works exactly as stock.

What we can work with:
- The module runs in idle time (the OS_Halt hook) and at VBlank (the CE9 ARM9 IPC IRQ hook, chained through `OS_IRQTable[16]`).
- DS games can't see the DSi NDMA channels. CE9 uses only NDMA0/1 for card reads.
- The module lives in DSi extra RAM at 0x02D80000 and has about 1.2 MB of heap.

## What RA actually requires during play

From the Hardcore Compliance Requirements and the rc_client integration wiki:

- **Section A:** "Measured and Trigger flags must work properly. Both must be visible in the achievement list as well as displayed during gameplay when appropriate conditions are met." This means challenge and progress indicators.
- **Section E:** the hardcore state must be visible during play, "normally done when the player starts a game".
- **Section B:** leaderboards can't be disabled in hardcore, but "disabling leaderboard popups is okay".
- **Not required:** unlock popups and sounds. The rc_client wiki leaves these to the client. RetroArch (approved) lets the user turn off unlock popups, challenge indicators, progress trackers and leaderboard trackers.
- **Not covered:** RA has no written rules for hardware clients, and nothing on non-visual or delayed cues.

On the DSi, every notice also inherits the evaluation backlog. rc_client events fire in idle time and can trail the frame they describe by up to about 17 s in heavy scenes.

## Options

### 1. Pre-boot placard (hardcore mode at game start)

**Mechanism:**
- The nds-bootstrap bootloader already owns both screens before the game boots, and already draws the ESRB and "please wait" screens (`retail/bootloaderi/source/arm9/errorScreen.arm9.c`, `main.arm9.c`).
- The DScheevos set parse already runs in that loop (state `ARM9_DSCHEEVOS_INIT`).
- Add a state that shows a 256x192 card for at least 2 s: title, Hardcore/Casual, unlocked/total and points, optionally a badge.
- The existing `clearScreen()` (`ARM9_SCRNCLR`) then wipes VRAM and the display registers before the game starts, as it does after the ESRB screen.

**Evidence:** code paths read in `external/nds-bootstrap`. The game isn't running yet, and the game starts from the same cleared state as stock.

**Risks per rule:** none. Boot takes up to 2 s longer when the parse finishes faster than that. One check: the DScheevos init path must never skip `clearScreen()`.

**Cost:** 1-2 days.

**Built** (`retail/bootloaderi`): after the module's init the ARM7 draws the card into the ESRB screen's buffer with the in-game menu's font (state `ARM9_DSCHEEVOS_PLACARD`): "RetroAchievements", the game title, HARDCORE or Casual (plus a line when hardcore was refused), unlocked/total achievements and points, and "Spectator: nothing is sent" in spectator sessions. It stays up 1.5 s from when it appears, counting the fade-in, while the ARM7 keeps loading; A, B or START skip it, and the screen is then cleared only once the key is let go (up to 2 s), so the game doesn't read it as a new press. `clearScreen()` runs after it unchanged.

**Test:**
- melonDS: take a screenshot during init. The first game frames must be pixel-identical to a run with the placard off.
- Hardware: boot Kirby and Pokemon Platinum once each.

### 2. In-game menu "Now" page (player-opened)

**Mechanism:** the first page of the L+DOWN+SELECT menu is built from rc_client's own buckets:
- active challenges
- almost there, with measured progress
- recently unlocked
- active leaderboards with their tracker text
- a ring of the last ~32 events

**Risks per rule:** it pauses only when the player asks, as the menu does today. It must never open by itself, because that would be an unrequested pause.

**Cost:** 3-5 days. Mostly shared with the full achievement list, which is needed anyway.

**Covers:** "visible in the achievement list" for Measured and Trigger. It does not cover "during gameplay".

### 3. LED cues (DSi)

**Mechanism:** this extends the existing CE7 Wi-Fi LED blink (`cardengine.c`, I2C BPTWL[30h]). The module publishes event counters, and CE7 turns their edges into LED changes:
- Wi-Fi LED steady on while a challenge is primed.
- 3 blinks on an unlock.
- 1 blink on a progress update.
- Power LED purple (BPTWL[63h]) while a leaderboard attempt runs.

**Risks per rule:**
- Nothing on screen, and nothing the game uses: DS-mode games have no I2C driver.
- Each edge costs an estimated 0.2-0.4 ms of ARM7 time. This isn't measured yet; it is the same order as the existing blink. Move the writes out of the VBlank IRQ if possible.
- Writes must keep the other BPTWL[30h] bits, which also gate the Atheros.
- Suspend Wi-Fi LED cues while the game's own DS wireless is on (CE7 already detects this). Skip the purple cue on low battery.
- DSi only: on a 3DS the CE7 blink function returns early.

**Cost:** 1-2 days. Test with an I2C write trace in the headless runner, and on hardware by filming the LEDs.

**Built, with changes** (`retail/cardenginei/arm7/source/cardengine.c`, header word `cues`):
- Leaderboards use the Wi-Fi LED too, not the power LED: 2 blinks per attempt started, failed or submitted. 3 per unlock, 1 for progress.
- A blink, and the hold while a challenge is primed, is the opposite of the LED's normal state: lit when it is normally off, dark when it is normally on (wireless enabled). "Steady on" would show nothing on a DSi with wireless on. Not yet signed off by the owner.
- Patterns queue, unlocks first. Progress is a level, not an event, so at most one progress blink waits.
- Off while nds-bootstrap's own "Wi-Fi LED on ROM read" option drives that LED. The LED goes back to normal before the in-game menu, resets and the return to the loader.

**Limit:** it tells the player *that* something happened, not *what*. Whether RA counts it as "displayed" is open (question 1 in `docs/ra-questions.md`).

### 4. On-screen top strip: engine A Main Memory Display mode fed by NDMA

This is the only on-screen mechanism found that writes no game VRAM, OAM, palettes or BG/OBJ/window/blend registers.

**Mechanism:**
- At VBlank, in the existing IPC IRQ, the module sets DISPCNT bits 16-17 to 3 (Main Memory Display) with a read-modify-write.
- It arms a DSi NDMA channel in startup mode 09h to feed the display FIFO (0x04000068) from a 256xN RGB555 banner buffer in module RAM: 4 words per block, (N+margin)*128 words, stopping by itself.
- Engine A shows the banner on lines 0..N-1. The mode is then switched back to the game's mode 1, so the game is drawn normally below the strip.
- The banner is rendered in idle time, only when a notice changes.

The design problem is **how to switch back mid-frame**. Four variants were tried:

| Variant | How the mode goes back to 1 | Proven in melonDS | Problem |
|---|---|---|---|
| 4a. NDMA H-Blank table | A second NDMA writes a whole 32-bit DISPCNT value captured at VBlank, in the H-Blank of line N-1. No IRQ. | Yes: line-exact, 0 lag. | NDMA writes are 32-bit only, so any DISPCNT change the game makes during the strip is overwritten for the rest of that frame, and stays lost until the game writes DISPCNT again. This was measured on real games: Kirby at line 7 (BG layers), Pokemon Platinum at lines 8/14, NSMB at line 12 (ext-palette bits). That is about 1-2 events per 2800 frames per game, clustered at scene changes. **Breaks rules 3 and 4.** |
| 4b. NDMA end-of-transfer IRQ, read-modify-write, then a one-word H-Blank NDMA write | The FIFO channel's IRQ computes the new value from the live DISPCNT and arms an H-Blank write. | Yes (the "S9" scenario): the game's mid-strip change is kept. | NDMA IRQs need SCFG_EXT9 bit 8, which the bootloader leaves clear before locking SCFG. The NitroSDK `OS_IRQTable` has only 22 entries (bits 28-31 are 0 in Kirby's DTCM), so the first NDMA IRQ would jump to address 0. The only fix is wrapping the IRQ vector at DTCM+0x3FFC, which is in game memory. melonDS doesn't gate bit 8, so it passes falsely. **Rejected.** |
| 4c. VCOUNT IRQ read-modify-write, with the NDMA table as a late-only backstop | The ARM9 VCOUNT IRQ at line N-1, chained through `OS_IRQTable[2]`, the way CE9's colour-LUT code already does it. It restores only bits 16-17 and then cancels the backstop NDMA, which sits M lines later. | Yes (phase 2 of the hook test), 0 lag. | The IRQ goes through the game's dispatcher, which writes the IF check word in game DTCM (the IPC hook already does the same). It sets IE bit 2 and LYC. nds-bootstrap's own `colorLutVCountBlacklist` lists 20+ titles that broke when the VCOUNT IRQ was hooked. Part of that is the colour-LUT handler writing master brightness, not VCOUNT itself, but it is still a warning. The game wins every conflict only if we back off whenever it owns VCOUNT. On hardware the write must wait for H-Blank (spin up to one line). |
| 4d. HBlank IRQ | Count lines in an HBlank IRQ. | Phase 3 of the hook test. | Costs about 4-7% of every frame in a game without HBlank IRQs. **Rejected.** |

**Evidence** (melonDS, DSi mode, experiment ROMs and patched runners kept outside the repo):
- `overlay-fifo`: 11 scenarios with a fake game. The game is pixel-identical below the strip. Mode-3 lines are dimmed by the game's master-brightness fades. Arming late is skipped by a VCOUNT 192-250 guard.
- `overlay-hooktest`: a game with 262 HBlank IRQs per frame. 0 lag frames and 0 late game IRQs with variants 4a and 4c.
- `overlay-irqtrace`: the real DScheevos nds-bootstrap chain with Kirby, NSMB and Pokemon Platinum. IPC IRQs entered at VCOUNT 192-231 in every observed case. The existing hook costs 750-880 ARM9 cycles per frame. 0-15% of frames got no IPC IRQ during card loads; on those frames the strip is simply missing, which is fail-safe.
- None of the three games used the main-memory FIFO or the ARM9 VCOUNT IRQ.

**Critic findings (checked against the sources):**
- **The whole option rests on one undocumented fact.** GBATEK lists NDMA startup 09h as "Work RAM (what?) (=probably Main memory display, as on NDS)". melonDS maps 09h to the FIFO by the same guess (`DSi.cpp` `NDMAModes`). So the melonDS results prove the design only if the hardware agrees. This is hardware test 1. If 09h doesn't feed the FIFO, the only other feeder is a legacy DMA in mode 4, and the legacy channels belong to the game. The option would then be dead.
- **Slowdown is not zero, even in melonDS.** In the hook test the fake game's workload took 3.9% longer (270k to 281k cycles per frame) while a 32+8-line strip was fed. That is about 1.9% of a frame. melonDS stalls the ARM9 for every DMA, so this is a worst-case model; real cost depends on cache and TCM hits. The display researcher's GBATEK estimate is 30-40% of the main-RAM bus during strip lines. A game running at its frame budget could drop a frame while a notice is up. This conflicts with rule 2 until hardware shows otherwise.
- **The game can see the strip.** For N lines per frame, DISPCNT reads back as mode 3. NitroSDK's `GX_DispOff` saves the current mode into its own variable (`sDispMode`, checked in the pokediamond decompilation). If it runs during the strip, every later `GX_DispOn` turns mode 3 back on with no FIFO data. Our register value then sits in game RAM, written by the game. The S10 scenario shows the result. It is rare, since real games change the mode mid-frame only around boot, but it is game state influenced by us (rule 4).
- **Starvation and underrun are unknown.** CE9's card NDMAs use 128-word physical blocks that outrank a later channel, and the game's legacy DMA has undocumented priority against NDMA. Either could starve the FIFO and corrupt the banner pixels. melonDS doesn't model this. The fix is to give the FIFO the highest-priority channel (NDMA0) and move CE9 card reads to NDMA1, or to shrink CE9's block size.
- **Bootloader SCFG write.** A test ROM writing the bootloader's DS-mode SCFG_EXT9 value (0x8300C000|BIT(16)) crashed under melonDS. The tests ran with the default DSi value, so the hardware run must use the real nds-bootstrap chain.

**What it looks like:** an opaque, full-width strip over the top 16-32 lines of the panel engine A drives (text, a 16x16/32x32 badge, a progress bar or leaderboard value). It can't be translucent. It hides the game's top lines, so notices are short (2-3 s), and trackers and challenge icons show briefly when they change instead of staying pinned.

**Cost:**
- Renderer (font, badge, buffers about 20 KB), notice queue and arming code: about 1 week.
- The VCOUNT hook (4c) and a per-game allow list: 2-3 days, plus testing many games.
- The hardware experiment alone: 1-2 days, since the ROMs exist.

### 5. Rejected

- **VRAM display mode (mode 2):** needs a VRAM bank the game may own (VRAMCNT is write-only).
- **Display capture blending:** needs a free 128 KB bank, adds a frame of latency, and fights games that capture (NSMB does).
- **Master-brightness bands or per-pixel tricks:** the register is the game's fade control (Kirby wrote it 3835 times in 9000 frames).
- **Borrowing a BG layer, OBJ/OAM entries, windows, blending, extended palettes, VRAM H/I or a 3D quad:** all game-owned, and the Bakkerrrs fork's OAM/VRAM overlay corrupted games.
- **Display-off white lines:** worse than option 4, with the same exposure.
- **NDMA IRQs and timers:** see 4b. Timers belong to the game.
- **Polling from idle time:** it can't meet a scanline deadline.
- **DSP chime:** needs a permanent 1/8 NITRO volume cut and SCFG changes.
- **TSC beep:** the codec is locked in NDS mode, and the SPI bus is shared with the game's touch reads.
- **Spare sound channel:** game state.
- **Backlight flashes:** whole-screen, and shared SPI.
- **Auto-opening the menu:** an unrequested pause.

### 6. Off-console and later targets

- **LAN companion display:** the module sends one UDP datagram per rc_client event to a phone or PC page. This costs nothing on the game side, and it is paused while the game's DS wireless is on, as all networking already is. About 1 week of work. Optional, and only counts for RA if RA says so.
- **3DS TwlBg overlay:** draws on the finished DS picture on the ARM11, with no contact with the game. This is the real graphical answer on 3DS. It takes weeks of reverse engineering and is noted in `docs/compliance.md`.

## Ranked recommendation

1. **Build the pre-boot placard (option 1) now.** It meets Section E with no in-game risk.
2. **Build the menu "Now" page (option 2)** together with the full achievement list.
3. **Build the LED cues (option 3)** as the DSi's only during-play cue that keeps every rule.
4. **Send the RA questions (`docs/ra-questions.md`) before building any on-screen DSi overlay.** If RA accepts 1+2+3 for the DSi, the DSi needs no on-screen overlay.
5. **Run the on-screen strip hardware experiment only (option 4).** Don't integrate it into the module yet. Even if it passes, it can't meet rules 2 and 4 strictly: there is a small main-bus cost while the strip is fed, and the game can read mode 3 for N lines. If the owner accepts those residuals:
   - ship variant 4c only;
   - make it opt-in per game, off by default;
   - use an allow list built from traces (no game-owned VCOUNT, no DISPCNT mode changes outside VBlank);
   - put the FIFO on NDMA0;
   - show it only after the display has been stable for a few seconds.

   Never ship variant 4a (proven clobber) or 4b (needs a write into game DTCM).
6. **On 3DS, the TwlBg overlay** is the graphical answer, later.

## Unknowns that need the DSi XL

Decide each pass/fail before the run and log it to SD.

1. **Does NDMA startup 09h feed the display FIFO at all?**
   Run the `overlay-fifo` ROM in DSi mode from TWiLight. Pass: banner on lines 0-15 and the game pattern from line 16-18, photographed.
2. **Does a mode 3 to 1 switch written in H-Blank take effect on the next line, with no torn line?**
   Use the same ROM, all scenarios. Pass: the edge is clean on 600 photographed frames.
3. **What does a FIFO underrun show?**
   Use scenarios S5/S6 (IRQs off). Record what appears; there is no pass/fail.
4. **Does a 128-word NDMA0 copy, or a legacy DMA3 burst, starve a FIFO NDMA?**
   Add a background copy loop to the test ROM. Pass: no banner pixel errors.
5. **What is the real bus cost?**
   Time a main-RAM busy loop with the strip on vs off. Pass: under 0.5% of a frame. This threshold is the owner's call.
6. **Does the bootloader's DS-mode SCFG_EXT9 value keep NDMA access working?**
   Run a forced banner through the real nds-bootstrap chain in Kirby, Pokemon Platinum and NSMB, 30 minutes each. Pass: no visual difference outside the strip in 240 fps camera capture, and an identical lag count against stock.
7. **LEDs: does the I2C cost per edge show up as ARM7 latency, and does the Atheros stay associated?**
   Pass: no audio glitch, and the router keeps the association.

melonDS can't settle these:
- bus arbitration and starvation
- mid-line register latching (it renders each line whole at H-Blank)
- SCFG_EXT9 gating of NDMA (bit 16) and of IE bits 28-31 (bit 8)
- NDMA timer start modes
- the LEDs

## Questions for RetroAchievements (RAdmin)

Moved to `docs/ra-questions.md` (expanded to 18 questions on 2026-10-12; it maps the old numbers).

## Owner decisions

- Is an LED-plus-menu DSi acceptable as the product even before RA answers? Or should DSi hardcore wait for RA's answer?
- Is the on-screen strip worth a hardware experiment, given that even a full pass leaves its residuals (main-bus cost while a notice is up, and DISPCNT mode 3 visible to the game for N lines)?
- If the strip ships: opt-in per game and off by default (recommended), what strip height, and is the VCOUNT IRQ hook acceptable?
- Is a 2 s boot delay acceptable for the placard?
- Is the LAN companion worth building?
