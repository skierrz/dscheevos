# Strip lab: the bottom notice strip in the module, in melonDS

Status: **lab only, 2026-10-20/21 (overnight).** Branch `strip-lab` (off `stack` 232fc929), worktree
`../dscheevos-striplab`. Nothing here is in a kit, on `perf`, or game-facing. It answers "does the notice strip
work inside real games?" in the emulator before any hardware run, as the owner asked.

## What was built

- `dscheevos/module/src/strip.c` (+ `strip.h`), compiled only with `make STRIP=1`. It links the display library's
  scheduler and renderer (`dscheevos/display`, unchanged) and moves the pixels with the D1 scheme from
  `docs/display-research/round2-synthesis.md`: an H-Blank NDMA writes whole DISPCNT words from a table (mode 3 on
  the strip's lines), a second H-Blank NDMA starts the main-memory display FIFO feed at line E-1, a third NDMA is
  the feed.
- **No code in the game's IRQ path.** D1 as designed aborts "first thing on any game IRQ" through a patched
  dispatcher. The owner has since ruled out anything in the IRQ path (read-only rule, 2026-10-13/17). The lab
  arms the strip only inside the module's last halt of an idle pass, which already runs with IRQs masked
  (CPSR.I, `module_idle` → `wake_wfi()`), and stops it the moment the CPU wakes, still masked, before the game's
  handler can run. So **no game instruction ever executes while DISPCNT holds our mode**.
- The tables are built in idle passes with IRQs on (one per strip height and game DISPCNT), so arming is ~20
  register writes with IRQs masked.
- **Arming in VBlank.** Many games finish their frame right after VBlank and stay halted until the next one. The
  strip is then armed in VBlank with the table starting at line 0; this relies on H-Blank DMAs not running during
  VBlank (GBATEK; display-lab H10/D1 checks it on hardware).
- **Stand-down.** After 30 frames in a row with no full strip, the module stops arming for 2 s (the scheduler
  holds the notice, and after 10 s puts it on the menu's missed list). Without this, games whose IRQs keep waking
  the halt cost the module every wake's full path for nothing (Kirby's intro: idle work 33k → 216k cycles a frame).
- Guards at arming: the game's DISPCNT still equals the value the table was built from and its display mode is
  1 (normal); NDMA0 (nds-bootstrap's card engine) and our three channels idle.
- `tests/strip-lab/`: build, run, screenshots every N frames (`FRAME SHOT` script lines), counters from the RAM
  dump, a sweep script.

## melonDS needs a one-line lab patch to show the strip at all

Stock melonDS decides once per frame, at line 0, whether the display FIFO runs (`GPU::StartFrame`, `RunFIFO`). A
strip switched on mid-frame gets no FIFO pixels: the band shows black. The lab runner (`melonDS` branch
`strip-lab`, worktree `../melonDS-striplab`, built at `private/lab/strip-runner`) re-checks per line when
`MELONDS_FIFO_PER_LINE=1`. **This is not a claim about the DSi.** Whether the DSi feeds a FIFO started mid-frame is
display-lab tests H10/D1/D1c, run on the owner's DSi XL.

## Results

Final build (`build-strip`: stand-down with back-off, game-DMA guard), lab runner with
`MELONDS_FIFO_PER_LINE=1`, `--profile`, no cache model; each game from TWiLight Menu++ through the launcher
(mock server, `tests/bootstrap/sets/pipeline.json`: session start, an unlock at 300 frames, mastery), about 25 s
from boot (logos, intros, title screens). "Full" = frames with the whole strip drawn, of frames with a notice to
show (armed + not armed + stood down). Lag = melonDS lag intervals of all intervals; game = the game's own ARM9
cycles per frame outside the module.

| Game | Full strip | Cut | Lag strip / base | Game cycles strip / base |
|---|---|---|---|---|
| Yoshi's Island DS | 647 / 648 (100%) | 0 | 39/1532 / 39/1531 | 48,527 / 48,551 |
| Phoenix Wright | 656 / 670 (98%) | 11 | 11/1507 / 11/1506 | 48,804 / 48,814 |
| Professor Layton (Curious Village) | 640 / 652 (98%) | 9 | 26/1511 / 26/1514 | 66,138 / 66,147 |
| Tetris DS | 633 / 652 (97%) | 13 | 29/1531 / 30/1534 | 43,894 / 43,875 |
| Digimon World DS | 603 / 621 (97%) | 12 | 78/1455 / 78/1459 | 141,041 / 140,888 |
| Pokémon Platinum | 619 / 640 (97%) | 19 | 30/1319 / 30/1320 | 74,380 / 74,319 |
| Zelda: Phantom Hourglass | 587 / 613 (96%) | 16 | 63/1528 / 63/1528 | 317,452 / 317,536 |
| Super Mario 64 DS | 529 / 651 (81%) | 97 | 153/1487 / 153/1490 | 239,104 / 238,914 |
| Pokémon Mystery Dungeon: Explorers of Sky | 480 / 628 (76%) | 125 | 169/1488 / 169/1488 | 334,302 / 334,310 |
| New Super Mario Bros. | 126 / 1287 (10%) | 25 | 54/1424 / 54/1423 | 377,956 / 377,764 |
| Mario Kart DS (two runs) | 2 / 1166 | 45 | 111, 106 / 107, 105 (103 in a third) | 206,556, 202,336 / 205,579, 202,919 (195,243) |
| Animal Crossing: Wild World | 0 / 900 | 0 | 455/1362 / 571/1370 | 623,449 / 630,837 |
| Nintendogs: Best Friends | 0 / 1297 | 0 | 135/1432 / 135/1440 | 432,639 / 432,369 |
| Kirby Super Star Ultra (intro video) | 0 | 0 | 445/1270 / 440/1261 | 699,110 / 698,825 |

Not run: Ghost Trick and Mario & Luigi: Bowser's Inside Story (TWiLight didn't start them in the harness),
Castlevania: Dawn of Sorrow and Metroid Prime Pinball (the base build failed the same check: module not active).

Module idle work (idle time only) per frame, strip vs base: +2-6k cycles in the served games; Animal Crossing
+16k, Nintendogs +17k with the back-off (+81k / +121k before it, probing every wake).

Part D check: the hardware-kit build (`build-strip-hw`, = `build-strip`; its status IRQ fields carry the strip
counters) with the 2026-10-20 kit's own launcher, loader and nds-bootstrap (df2df46c): Phoenix Wright PASS, 656
full / 11 cut, lag 11/1313; Kirby PASS, no strip, lag 447/1275.

## What this means

- **The mechanism works inside real games in melonDS**, with no code in the game's IRQ path: 7 of 13 games show
  the whole strip on 96-100% of the frames that had a notice, 2 more on 76-81%. Lag frames and the game's own
  cycles are the same as without the strip, within run noise (Mario Kart DS's three base runs spread 195-206k).
- **Not served under the strict rule:** games whose IRQs wake the idle thread before the strip's lines on nearly
  every frame (Kirby, Animal Crossing, Nintendogs, Mario Kart DS races). Serving them needs either a check after
  the game's IRQ handler (IRQ-path code: ruled out) or letting their handlers run while the mode is switched,
  where a game write to DISPCNT could be undone by the next table write. Neither fits the rules. They keep the
  LED and the menu.
- **Games with their own H-Blank / display-FIFO / geometry-FIFO DMA** keep the strip off (NSMB's title screen):
  their DMA destinations can't be read back, and one aimed at DISPCNT would fight the table.
- **Cuts** (an IRQ inside the strip's lines; the rest of that frame's band is the game's picture) are 1-3% of
  frames in most served games and 15-20% in SM64DS and Mystery Dungeon: our band flickers in height there. The
  game's pixels are never touched.
- **Owner rulings needed:** (1) the DISPCNT mode write while the game is halted (the one write no on-screen
  notice can avoid); (2) confirm Kirby-type games stay LED + menu; (3) cut flicker acceptable, or end a notice
  early in games that cut often (display-library R19).

## Not covered

- Real hardware: bus contention (H5), ARM7 audio under the FIFO (A7), whether the DSi feeds a mid-frame FIFO
  (H10/D1/D1c), H-Blank DMA during VBlank. melonDS has no bus model, so "no added lag" here is necessary, not
  sufficient.
- Gameplay scripts: the runs cover each game's first ~25 s after boot (logos, intros, title screens).
- Cache effects (runs without `--cache-model`).
- Card reads during the strip: guarded only by NDMA0's busy bit at arming.
