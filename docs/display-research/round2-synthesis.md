# Round 2 synthesis: on-console indicators for every game

Date: 2026-10-12. Opus propose → red-team → choose. Proposals and red-teams are in `round2/`:
- `prop-space` / `rt-space`: other engine, capture, VRAM banks;
- `prop-time` / `rt-time`: idle windows, raster dodging, blank frames;
- `prop-dsi` / `rt-dsi`: DSi hardware, the ARM7, the DSP;
- `prop-sdk` / `rt-sdk`: hooks in the game's own SDK.

The brief is `round2/BRIEF.md`.

## The bar (owner)

- On-console indicators for **100% of games with RA sets**: challenge (Trigger), progress (Measured) and unlocks.
- **No lag frames.** Any cost in the game's working time must be bounded, measured and as small as possible.
- **Zero visual compromises:** no garbage, tearing or flicker, and nothing the game draws damaged.
- **No quiet exclusions.** A class that can't be served is reported with evidence.

A newer merge rule (COMMON.md "Game-cost evidence") also counts the game's own cycles: cache pollution is a cost. The baseline debt is already Kirby +21%.

## What survived the red-teams

| Idea | Verdict | Why |
|---|---|---|
| **D1: an all-NDMA bottom strip.** H-Blank-started NDMA writes whole DISPCNT values from a table; a second NDMA starts the FIFO channel; no CPU and no module IRQ while it shows. The game never sees mode 3, because the patched dispatcher stops the DMAs and restores mode 1 before any game handler runs | **Chosen as the core**, with rt-dsi's four blocker fixes:<br>• abort-check address: channel A at 0x04004154, plus a VBlank fail-safe;<br>• atomic arming (IRQs off, a generation counter);<br>• poll A's busy bit before the restore;<br>• no cache restore while an IRQ ran or a DMA is busy | No CPU per line and no IRQ races. The DISPCNT exposure (R1/R2/R20) goes to zero. Cost in working time: about 13-60 cycles per game IRQ while armed, 300-1000 per shown frame (rt-dsi estimate, to measure) |
| Idle gate, "the game waits for VBlank by line E" | **Kept**, as rt-zero-lag 11.4 | The only way to keep the strip's bus time out of the game's working time |
| **BIOS-wait signal** (idle-coverage's hook) | **Kept** for class C | VBlankIntrWait is an exact "waiting for VBlank" |
| **Abort on the reschedule flag**, not on every IRQ | **Kept**, needs a disassembly check per SDK variant (rt-time B1) | Every-line H-Blank games wake their VBlank thread every line only to re-sleep. Aborting on every IRQ serves them 0% |
| Per-game strip window (T1) and whole-notice scheduling (T5) | **Kept, weakened** | Coverage swings with game phase; menu-run numbers overstate it (rt-time) |
| Power LED purple for the *persistent* challenge state (`display-leds.md`) | **New in synthesis** | rt-time B2: on-screen persistent indicators blink wherever idle windows break. A held LED colour doesn't, and challenge start/end still get on-screen notices |
| NWRAM as the FIFO source | **Open** | GBATEK says the source "must be in Main Memory"; `dsiWramAccess` depends on how the game is launched; ROM spill and FNT/FAT use NWRAM. Hardware test |
| D7, cache snapshot and restore | **Deferred** | Correctness hazard (rt-dsi B4). Kept small instead: the strip's per-frame footprint is a few hundred bytes of code and data |

**Rejected, with the red-team that sank each:**
- P2v, a VRAM-display band from a "free" bank: free only *now*, and wrong guesses corrupt (rt-space B1).
- Running anything while the game works without a bound (rt-space B2).
- S2, a persistent sprite: capture copies it into game VRAM, and unhooked mapping changes give persistent garbage (rt-sdk B1, B2).
- S1, a per-frame sprite append: still possible, but it costs the game's OBJ line budget, which melonDS can't show (rt-sdk M1). Parked.
- Every-Nth-frame and split notices: flicker.
- Capture compositing, the 3D engine, engine-B borrowing: game state.
- The DSP doing DISPCNT read-modify-writes: async clobber, not enabled in DS mode (rt-dsi).

## Coverage by class

| Class | Examples | Mechanism | Status |
|---|---|---|---|
| Idle by the strip's lines most frames | Phoenix Wright, Tetris, Digimon, Ghost Trick, Yoshi's Island | D1 + idle gate | **Served**, pending hardware H1/H10/T1 |
| C: BIOS-wait | Sonic Rush, RE:DS, Super Princess Peach, Metroid Prime Pinball | D1 + idle-coverage's BIOS-wait signal | **Servable** when the hook exists; 54-89% of frames idle, so persistent state goes to the LED |
| B: every-line raster IRQs | Kirby SSU, Pokémon Black, Mario Party, Nintendogs, Starfy | D1 + reschedule-flag abort | **Servable if** the flag check holds per SDK variant (disassembly + test) |
| B': a raster IRQ at a fixed line | Bowser's Inside Story (168), Clubhouse (191) | per-game window that avoids that line (T1) | **Servable**, measured per game |
| A: busy at the bottom | **Mario Kart DS races**, Diddy Kong Racing races, Cory, 999 | see below | **Not served within the bar today** (evidence below) |
| E: screen swap every frame (dual-screen 3D) | Clubhouse (some modes), Pac'n Roll, Peggle, LEGO SW | none | **Not served within the bar** (evidence below) |
| D: never waits | Golden Sun DD, Code Lyoko | none for display; DScheevos itself gets no time today | **Not served** (idle-coverage's busy-loop work first) |
| Phases with no idle (cutscenes, FMV, loads) | Layton FMV, MvDK 33-s cutscene | notices wait for the next idle stretch; LED meanwhile | Delayed, not lost. Owner: is a delay acceptable? |
| F: DSi-enhanced in DS mode | Pokémon Black | as B | Memory budget to confirm |

### Evidence for "not served within the bar"

**Class A: Mario Kart DS and Diddy Kong Racing races.** These are 3D games that work through the bottom of every frame.
- **Idle by line 156:** 0% (census). By 182-189: MKDS 53% of frames, DKR 11-35%, in broken runs (`round2/time/best.txt`, re-derived in rt-time).
- **Every lever, and why it fails:**
  - an **idle-gated strip** (D1, P2b): no idle at the band;
  - a **strip while the game works**: costs bus in its working time; the owner rejected unbounded risk, and rt-dsi estimates ~1.4% of a frame on MKDS, unmeasured;
  - a **VRAM-display band**: needs a free bank, and 3D games use theirs for textures (rt-space B1);
  - a **sprite**: OBJ budget and capture (rt-sdk);
  - **engine B**: game state;
  - the **top edge**: MKDS never finishes inside VBlank (best.txt, top column 0%);
  - the **LED** works, but isn't on-screen.
- **What would serve them** (owner decision): accept a **bounded, measured working-time cost** for this class. D1 during work, with the FIFO from NWRAM if possible, costs about 1-1.5% of a frame of bus *only on frames a notice shows*. It needs proof by T1-style measurement that the game's per-frame slack exceeds it. Without that ruling the class gets the LED and the in-game menu only.

**Class E: games that swap screens every frame.**
- A strip on engine A appears on one physical screen per frame, which is a 30 Hz flicker (rt-space, rt-time).
- **The only lever,** stamping the band into the game's capture bank (D6/E-stamp), writes game VRAM. rt-space M3/M4 found it needs idle time on both halves of the frame pair, and could leave our band in a frozen captured frame.
- **What would serve them** (owner decision): accept writing our band rows into the game's capture bank in idle time, restored when the notice ends, as a residual. Otherwise the LED and menu only.

**Class D: Golden Sun DD and Code Lyoko.** They never wait, so DScheevos itself gets no CPU today. The display is moot until idle-coverage's busy-loop work gives the module time.

## NDMA channels (round-1 rnd conflict, settled 2026-10-12)

nds-bootstrap's ARM9 card engine (CE9) starts its transfers on **NDMA0** (`ndmaCopyWordsAsynch(0, ...)`) and checks **NDMA0/1** for busy. DS-mode games can't use NDMA at all. The display therefore takes:

| Channel | Use | Notes |
|---|---|---|
| NDMA2 | D1_F: the FIFO feed (startup 09h) | Lower priority than CE9's NDMA0, but the strip never runs during a card read (guard), so they never compete |
| NDMA3 | D1_A: the H-Blank DISPCNT table | |
| NDMA1 | D1_B: the H-Blank write that starts D1_F at line E-1 | **Only if needed:** if the DSi drains the FIFO only on mode-3 lines (H10a / D1c), D1_F is enabled at arming and NDMA1 isn't used. If needed, it is shared with CE9 by construction: the strip arms only while CE9 is idle (NDMA0/1 not busy, no read in flight), and anything that could start a card read first wakes the game, which aborts the strip and frees NDMA1 |
| NDMA0 | CE9 only | never touched by the display |

idle-coverage's wait gate must not use NDMA1-3.

## Chosen build order (nothing game-facing before the owner's rulings)

1. **Hardware first.** The DSi lab-ROM run (already batched with the owner):
   - H1 (FIFO feed);
   - H10 (bottom strip entered mid-frame);
   - T1 (slack probe);
   - L1 (power-LED colours).
   - Plus a new **D1 test** (an all-NDMA DISPCNT table, no CPU) and an NWRAM-source test. Both are added to the lab ROM before the run.
2. **Data, with the corrected melonDS** (`--thumb-cycle`, cache model):
   - rerun the idle census on **gameplay** scripts, not menus;
   - a per-line, thread-aware census (rt-time);
   - disassembly checks of the reschedule flag per SDK variant.
3. **D1 in the display library** behind the same hardware-ops interface as `dsd_strip.c`, host-tested in the line simulator, with its game-cost footprint reported per the new merge rule.
4. **Owner rulings needed:**
   - (a) class A: a bounded working-time cost?
   - (b) class E: the capture-bank stamp?
   - (c) the power LED as the persistent challenge indicator, if L1 shows it's visible;
   - (d) notices delayed through cutscenes and loads;
   - (e) whether an opaque band over game pixels for a few seconds counts as "no visual compromise" (rt-space M6). Every on-screen design here covers some game pixels.
