# Prior art: overlays alongside running DS games (facts only)

Scope: facts with citations. No recommendations. Items marked NOT FOUND mean the search did not locate a source; they are not claims of absence.
Not repeated: contents of docs/overlay-research.md.

## 1. Bakkerrrs/Retroachievements-DS-on-3DS

Repo: https://github.com/Bakkerrrs/Retroachievements-DS-on-3DS (GPL-3.0 fork of nds-bootstrap, "nds-bootstrap-ra"; ~12 commits, 16 stars at fetch time).
Key files:
- Dev notes: https://raw.githubusercontent.com/Bakkerrrs/Retroachievements-DS-on-3DS/main/docs/retroachievements.md (~9500 lines)
- `retail/common/include/ra_overlay.h`, `retail/cardenginei/arm9/source/ra_overlay.c` (per that doc, lines ~1861-1862)
- `docs/twlbg-overlay-proposal.md`, `docs/ra-registration-request.md`, `docs/devlogs/2026-08-04.md` ... `2026-08-23.md`
- Scope: 3DS only; on DSi or flashcart it behaves like upstream (README).

How it draws (retroachievements.md, "How it works" section):
- Runs in the cardengine ARM9 inside the game's address space.
- Borrows from the SUB engine only: one BG layer that the game currently has disabled, one 16 KB character block of sub-BG VRAM (tiles at the start, map 2 KB in), and palette bank entry/entries (originally all 16 entries of bank 15, later cut to the single entry used).
- Sets the borrowed layer to priority 0, enables it, and re-surveys BGCNT/DISPCNT every frame. If the game starts using the block, it hands it back.
- Draws for 180 frames; cost measured with `linesLast`/`linesMax` scanline counters (up to 11 lines seen, with the in-game menu open, which they say perturbs the reading).
- Counters `shows`, `denied`, `deniedNoLayer`, `evicted`, `rearmDispstat` are published in `raSnapshot` (magic `RA2S`).

What failed / glitches reported (all in retroachievements.md):
- Final Fantasy III: game fades by enabling all sub BG layers, so overlay was denied. `deniedNoLayer` equalled `denied` in both samples (1/1 at 4,411 frames; 4/4 at 7,782). VRAM was never the reason. Unlocks must be queued.
- Same game: the overlay appeared on the top screen in the field and the bottom in battles. The sub engine's panel is POWCNT1 bit 15 (display swap), which the game owns; the overlay did not read it.
- Palette bug: overwrote 16 entries of palette bank 15 while only 1 was needed; showed as a transient fault on the title screen. Fixed to borrow a single entry ("still one the game may be using").
- Chosen VRAM block stopped being free: a game moved its BG2 character base onto it mid-play, and a repair loop that rewrote tiles each frame was destroying the game's tiles. Also BG0 was assumed free but the game enabled it with its own character base.
- Contra 4: notification drawn but invisible, with the game's bottom-screen tiles flickering while it was up ("section: A second category of overlay failure", ~line 5096). BG extended-palette hypothesis was tested (`overlayState` 0x08, ext palettes off) and ruled out. Later runs showed it appearing, flickering rapidly, which they attribute to scene dependence and `rearmDispstat` = 71 (the game rewrites sub display registers constantly).
- Ketsui Death Label: banks A=0x83 (3D texture), B=0x8B (3D texture), C=0x84 (sub BG), D=0x84 (sub OBJ), E=0x83 (texture palette), F/G=0x80 LCDC, H/I disabled. Per twlbg-overlay-proposal.md, "nowhere on that console's DS hardware to draw that can be shown to be safe"; the popup is opt-in per game and off by default.
- Known interaction left unfixed: pressing X to open the in-game menu while a notification is up caused a graphical fault (both write sub-engine registers).
- `surveyBlocks()` bug with bitmap-mode BGs (listed in the open items).
- DS VRAM ignores 8-bit writes; the cardengine `.bss` is never zeroed (use magic-guarded state).
- The doc states "Confirmed on hardware. Text draws over a running DS game on a 3DS, without pausing it, and the game keeps running normally underneath." Space Invaders Extreme: 4 shows, 0 denied, 1 evicted. Super Mario 64 DS: 6 shows in 1291 ticks, seen.
- The overlay lacks a real font (11 hand-drawn glyphs in message order); a full font does not fit the cardengine, so a separate ARM9 binary `cardenginei_arm9_ra` was planned.

TwlBg proposal (docs/twlbg-overlay-proposal.md):
- Asks the TWPatcher/RTCom authors for a TwlBg that draws text over the DS picture.
- Cites GBATEK: LgyFB output "must be DMAed to memory"; ARM11 + CDMA (channels 0Dh/0Eh) do the transfers; LGYFB_0 (0x10110000) is the bottom screen, LGYFB_1 (0x10111000) the top; LGYFB_ALPHA at +0x20.
- Publishes a snapshot interface in DS main RAM (`notifySeq` at +0xE0, `notifyId` +0xE4, `notifyHardcore` +0xE8, `notifyLen` +0xE9, `notifyTitle[64]` +0xEC; snapshot at 0x027FEA00 on retail DS titles). Fallback channel: RTCom.
- States the author cannot obtain or analyse the TwlBg binary; unresolved: where TwlBg keeps the buffer, and whether the ARM11 can read DS main RAM (cites 3dbrew: legacy ARM9 "loads the TWL launcher located at physical address 0x27C00000, which was written there by the TwlBg ARM11 process").
- No reply found in what I read (devlogs not read in full).

RA relations (docs/ra-registration-request.md, draft, "Not sent"): client sends an unknown User-Agent so the server blocks hardcore (unlocks filed as softcore). States "There is no documented registration form for a client that is not an emulator". Also describes deferred unlock submission, with `o=` for earn time.

## 2. DS cheat engines / in-game menus

- nds-bootstrap in-game menu: opened with L+DOWN+SELECT (configurable). Has screenshot mode, RAM viewer, RAM editor, "R: advance by one frame" (suggests the game is held while open; the page does not say so outright), return to loader (not in B4DS; may fail on some O3DS). Sources: https://wiki.ds-homebrew.com/nds-bootstrap/controls. Cheats are chosen in TWiLight Menu++ per-game settings, not in this menu.
- NitroHax: cheat engine hooked into a regularly-called function, by default the ARM7 VBlank handler; hook address can be changed by a custom code. Menu runs before the game boots (https://www.gamebrew.org/wiki/NitroHax). Not an overlay.
- Action Replay DS: a forum comment says the master code installs a kernel in unused RAM called each VBlank (https://nesdev.nes.science/f5/t2922.xhtml; unverified). DSATM dropped ARM9 hooking because ARM7 hooking "works 100% of the time" (https://www.gamebrew.org/wiki/DSATM). Cheat-code reference: https://doc.kodewerx.org/hacking_nds.html.
- Analog-Controls-for-NDS-Games-on-3DS: copies ARM9 code to a "safe" place and branches into it from the game's input routine (https://github.com/shocoman/Analog-Controls-for-NDS-Games-on-3DS). It does not touch the framebuffer (per Bakkerrrs' doc).
- NOT FOUND: sources on CycloDS, Acekard, R4/DSTT, EZ-Flash in-game menus or real-time-save menus, and on any cheat/menu product that shows an overlay without pausing.

## 3. Homebrew DS overlays / screen tricks

- NOT FOUND: any project besides the Bakkerrrs fork (item 1) that draws on a running commercial DS game on real hardware. No$gba debug message and DS screenshot-tool mechanics were not researched in this pass.

## 4. 3DS

- TWPatch / TWPatcher: patches TWL_FIRM's TwlBg to change scaling filters, widescreen and deblur; generates `TwlBg.cxi` that Luma loads (external FIRMs/modules must be enabled in Luma). TWiLight Menu can load a different TwlBg per game ("Widescreen.cxi"). Sources: https://consolemods.org/wiki/3DS:TWPatcher, https://wiki.ds-homebrew.com/twilightmenu/playing-in-widescreen, https://gbatemp.net/threads/_.543291. A twl_firm_patcher repo (https://git.randommeaninglesscharacters.com/ahezard/twl_firm_patcher) is described in search results as keeping only a "deblur" patch, and older builds as having unlocked DSi-mode hardware features in DS mode; the page itself failed to load, so this is from search snippets only.
- 3dbrew Legacy FIRM PXI: https://www.3dbrew.org/wiki/Legacy_FIRM_PXI. TwlBg (ARM11) writes the TWL launcher to 0x27C00000; LgyP9 runs on ARM9. The page does not mention LgyFB, overlays, Luma or Rosalina.
- Rosalina in TWL/DS mode: NOT FOUND. Searches returned no statement either way. (Rosalina is documented as an overlay menu opened with L+Down+Select by default: https://github.com/LumaTeam/Luma3DS.)
- No project other than Bakkerrrs' proposal was found that draws on top of DS games on 3DS.

## 5. Other hardware / clients and RA statements

- MiSTer: odelot/Main_MiSTer fork (https://github.com/odelot/Main_MiSTer). Unlocks show an OSD notification and are sent to the RA server; a key press opens an overlay listing achievements (https://retrohandhelds.gg/mister-gets-retroachievements-support-for-snes-n64-ps1-and-more/). Hardcore initially absent; NES hardcore announced (Nepiki quote); Wes Copeland said RA is "actively pursuing hardcore verification for MiSTer" (https://www.timeextension.com/news/2026/04/wow-that-was-fast-hardcore-retroachievements-support-comes-to-mister-fpga). Implementation details of the overlay code were not read.
- Analogue Pocket, EverDrive/Krikzz: NOT FOUND (no RA support sources located).
- RA hardcore compliance page (https://docs.retroachievements.org/general/hardcore-compliance-requirements.html): hardcore state must be visibly indicated, usually a brief message at game start; leaderboard popups may be disabled; Measured and Trigger flags must be visible in the list and during gameplay when the conditions apply; achievement list must be easy to open. "The page doesn't cover hardware or non-emulator clients"; every requirement refers to emulators. Eligibility: emulator (or parent) public for at least 6 months. Section G auto-fail criteria exist. Also: a 2025 RAdmin post on a revamped approval process (https://retroachievements.org/forums/topic/31930). No official statement on hardware clients found.
- Bakkerrrs' draft states RA has no documented process for non-emulator clients and has not been sent.

## 6. DSi-specific

- NOT FOUND: any homebrew using DSi NDMA, or the main-memory display FIFO, for display in DS mode. The GBATEK fetch failed (host unreachable), so the NDMA startup-mode 09h text was not re-verified here. See docs/overlay-research.md for the existing GBATEK/melonDS reading.
- The twl_firm_patcher snippet above mentions DSi-mode hardware features unlocked in DS mode on 3DS (unverified).

## Gaps
Searches were shallow for items 2, 3, 5 (Pocket/EverDrive) and 6. Bakkerrrs devlogs (2026-08-*.md), docs/retroachievements.md beyond the overlay sections, and the odelot source were not read in full.
