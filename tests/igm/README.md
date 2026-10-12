# In-game menu round trip

`run.sh` checks that nds-bootstrap's in-game menu hands the game back what it took. Through the launcher flow (`tests/launcher/run.sh`, DScheevos session on unless `ENABLED=0`) it opens the menu (L+DOWN+SELECT) and closes it (B) twice: 300 frames, then 900 frames. 120 frames before each open, the runner writes known values into the sub engine's BG scroll, blending and mosaic registers and the ARM7 master volume (`--poke`). This stands in for a game that set them once and left them. The runner logs these registers and the game's counters after every frame (`--trace-io`), and `check.py` compares the frame two before the game freezes with the first frame after it runs again.

```sh
source ../dscheevos.env
~/Developer/DScheevos/bin/emu-lock -l igm tests/igm/run.sh kirby
ENABLED=0 ~/Developer/DScheevos/bin/emu-lock -l igm tests/igm/run.sh pokemon   # nds-bootstrap's own path
```

The menu's open time is measured as the span in which the SDK's VBlank counter (`0x027FFC3C`) stands still. In that span both CPUs sit in the game's IRQ handlers with interrupts off. "Overhead" is that span minus the frames between the hotkey and B.

The game's OS tick counter (`OSi_TickCounter`, a static in the game's ARM9 binary) is found by a first, shorter run: `find_tick.py` looks for the word that grows by 7.99 counts a second between three RAM dumps. Set `TICK=0x0...` to skip that run.

## Checks

| Check | Passes when |
|---|---|
| `bgNhofs_b`, `bgNvofs_b` | every BG the game shows as a text BG keeps its scroll. A BG the game doesn't show, or shows as an affine BG, is listed as `n/a`. A register the game rewrites every frame is skipped, since it proves nothing |
| `bldcnt_b`, `bldalpha_b`, `bldy_b`, `mosaic_b` | unchanged |
| master volume | SOUNDCNT bits 0-6 unchanged |
| VBlank count | grew by no more than 3 over the menu and the 2 frames before it: the game saw one late VBlank, not the hundreds that passed |
| OS tick | grew by less than 2 overflow periods plus 3 frames (300 ms), however long the menu was open |

## Timekeeping: why the game should see a pause

While the menu is open the game is stopped. Both CPUs sit inside one of the game's IRQ handlers with IME=0: the ARM9 in its IPC-sync IRQ, the ARM7 in its VBlank IRQ. The hardware keeps running. Every VBlank sets the VBlank bit in IF, and every TIMER0 overflow (each 125 ms at the SDK's 64-cycle prescaler) sets the timer bit. IF holds one bit per source, so on exit the game takes one VBlank IRQ and one timer IRQ, whatever the menu's length. Its VBlank counter and its OS tick (`OSi_TickCounter`, which counts TIMER0 overflows in software) each move by one, plus the timer's live low 16 bits.

There are two things the game could be shown instead:

1. **Catch up**: the counters jump by the real time the menu was open. That can't be done here. The counters are the game's RAM, and the only other way to move them is to run the game's IRQ handlers N times. That runs N frames' worth of game IRQ code (DMA uploads, input, sound ticks) in a burst, which no game expects. Even if it were possible, it would be the wrong model. Games that use OS_GetTick deltas for timeouts, animation or physics would see a multi-second step. Games that count VBlanks to catch up on frames would fast-forward through them. SDK alarms (TIMER1) would all fire at once.
2. **A pause**: the game sees time stand still. This is what the DS itself does when it sleeps, the only "pause" a retail game knows about. With the lid closed, the SDK's sleep mode stops the main oscillator, so timers and VBlanks stop. When the game wakes, OS_GetTick and the VBlank count go on from where they were. Only the RTC has moved. Games are written for this.

The menu already gives the game a pause. The check confirms that the tick and VBlank count move by the same bounded amount for a 300-frame and a 900-frame menu. The pause isn't exact. With an overflow pending the tick moves by 65536 minus the timer's phase at entry plus its phase at exit, 0 to 250 ms of game time, about 125 ms on average, and the VBlank count by one. That jitter could be removed only by stopping, reloading or reading the game's TIMER0 and clearing its IF bit, which the project rules forbid (no game TIMER registers, no game state). It's the size of one hitch a game sees after any long lag frame.

What DScheevos sessions add: the menu releases the game at the start of a VBlank (line 192), right after the ARM7's wait for VBlank (`unloadInGameMenu`). The game's pending VBlank IRQ then runs during a real VBlank, so its VBlank-time VRAM/OAM uploads happen when they should, and the resume looks like one ordinary late frame. On nds-bootstrap's own path the release comes after 80 KB of SD paging, somewhere mid-frame.
