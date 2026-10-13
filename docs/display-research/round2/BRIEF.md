# Design round 2: on-console RA indicators for 100% of games (brief)

## Goal (owner, firm)
RetroAchievements indicators on the console for EVERY DS game with an RA set:
- the challenge (Trigger) indicator, while active;
- the progress (Measured) indicator;
- unlock notices.

Conditions:
- **Nil lag:** no lag frames; any cost in the game's working time bounded, measured and as small as possible.
- **Zero visual compromises:** no garbage, no tearing, no flicker, nothing the game draws damaged.
- **No quiet exclusions.** A game one mechanism can't serve needs another mechanism. A class that truly can't be served within the rules must be reported with hard evidence (each lever tried and why it fails).

Target: DSi (and DSi XL) running DS games natively through nds-bootstrap. 3DS later (TwlBg overlay, out of scope here).

## Hard project rules
- Games run natively.
- Never touch the game's RAM. Game-owned VRAM, OAM, palettes and display registers count as game state; any use is a residual the owner must accept explicitly.
- No SD writes during play; SD reads only through nds-bootstrap's path.
- No game TIMERs.
- rcheevos unmodified.
- Game wireless stays stock.
- Nothing writes NAND, firmware or the Wi-Fi EEPROM.

## Where we are (read these; repo = ~/Developer/DScheevos/dscheevos-display, branch display)
- **The current design** is a bottom-edge strip: engine A's Main Memory Display (DISPCNT mode 3) on lines 158-189, fed by DSi NDMA. It is shown only while the game waits for VBlank, aborted by any game IRQ, and leaves a cycle bound X to be measured on hardware. See:
  - docs/display-library.md (sections 6, 11, 11.4);
  - dscheevos/display/src/dsd_strip.c (built, host-tested).
- **Evidence:**
  - docs/display-research/hardware.md: every display mechanism, with GBATEK citations;
  - in-game-menu.md;
  - red-team.md and red-team-zero-lag.md;
  - survey.md: the M1 melonDS survey of 45 titles (DISPCNT access by line, raster IRQ lines, capture use, IRQ handler variants);
  - allow-list.md: the strip qualifies for only 5 of 22 SDK-idle games that reached play.
- **idle-coverage's census** of how games wait for VBlank: git -C ~/Developer/DScheevos/dscheevos-idlecov show idle-coverage:docs/idle-census.md.
  - Classes: oshalt 53, bios (SWI 5 VBlankIntrWait) 9, busy 2.
  - Per-frame raw data: ~/Developer/DScheevos/private/census/out/<game>/frames.csv (wait_line, wait_kind, wait_run per frame).
  - idle-coverage is building hooks for BIOS waits and busy loops, and works on the ARM7 side.
- **Timing caveat:** stock melonDS runs Thumb ~2x too fast on I-cache hits, so measured slack is optimistic. eval-speed's runner has --thumb-cycle (melonDS branch dscheevos-evalspeed). Hardware (the display-lab ROM, tests/display-lab, docs/display-hw-tests.md) decides.
- **GBATEK:** (local scratch)
- **Sources:**
  - nds-bootstrap: ~/Developer/DScheevos/dscheevos/external/nds-bootstrap;
  - melonDS: ~/Developer/DScheevos/dscheevos/tools/melonDS;
  - module: ~/Developer/DScheevos/dscheevos/dscheevos/module.

## Who the strip doesn't serve (the classes to solve)
- **A. Busy at the bottom:** the game still works at lines 156-189 (Mario Kart DS 100% of frames, Cory, Diddy Kong, 999, Layton partly). Its idle window may sit elsewhere in the frame or be tiny.
- **B. Raster IRQs on the strip's lines:** H-Blank or VCOUNT IRQs every frame (Kirby Super Star Ultra, Pokémon Black, Animal Crossing, Starfy, M&L BIS, Trauma Center 2, Clubhouse Games).
- **C. BIOS VBlankIntrWait games** (Sonic Rush, RE:DS, Super Princess Peach, Metroid Prime Pinball, ...): the module gets no time today (idle-coverage's hook coming).
- **D. Busy games that never halt** (Golden Sun DD, Code Lyoko).
- **E. 3D and dual-screen-3D games** that flip POWCNT1.15 (screen swap) per frame.
- **F. DSi-enhanced games** run in DS mode (memory constraints).

## Output wanted from a proposer
Concrete mechanisms, each with:
- exactly how pixels (or another indicator) reach the player: registers, timing, which CPU;
- which game state it touches (if any) and why that's within the rules, or the residual it needs;
- the worst-case cost in the game's working time, and how it's bounded;
- which classes and games it serves, and why;
- the risks and the experiment (melonDS with --thumb-cycle, or DSi display-lab) that would prove or kill it, with a pass mark.

Rank them, and say which combination covers 100%. Name any class you believe can't be served, with the evidence. Be concrete and cite sources (file:line, GBATEK section). No product code, no repo edits, no emulator runs.
