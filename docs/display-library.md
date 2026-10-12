# DScheevos display library: design

Status: **design proposal, 2026-10-12**. No product code yet. Every effect a game could see is listed in "Residuals" for the owner to rule on: under the project's rules each one blocks until accepted.

Evidence lives in `docs/display-research/`:
- `hardware.md`: every way to put pixels on a DSi, with GBATEK and source citations.
- `in-game-menu.md`: how nds-bootstrap's menu takes over the screens, and what it costs.
- `prior-art.md`: facts only.
- `assets-and-ux.md`: facts only.

`docs/overlay-research.md` is the earlier round. This document supersedes its ranking.

## 1. What we learned (short)

1. **A takeover pop-up is out.** Using the in-game menu's method costs about 70-136 lag frames per pop-up: the game must be paused, the menu's SD paging and handshakes add 10-16 frames on top, and write-only registers can't be restored. All eight cheaper variants checked also fail. Only a pause the player asked for (the menu) can work this way.
2. **Nothing on screen is strictly zero-cost on a DSi.** Every pixel path goes through engine A's display-mode bits (DISPCNT A bits 16-17) or through engine B's own layers, objects, VRAM and palettes. The lowest floor found is one byte of DISPCNT for a few lines per frame.
3. **Borrowing the game's sprites or BG layers fails on hardware.** The Bakkerrrs fork did it with per-frame detection and logged corruption on several titles:
   - Ketsui: garbage graphics.
   - Contra 4: flicker.
   - Final Fantasy III: refused every time.
4. **The prior strip design leaked more than it said.** It set mode 3 in the VBlank IPC IRQ (lines 192-231), so the game could read mode 3 for about 70+N lines, during the stretch where NitroSDK runs `GX_DispOff`. Its VCOUNT-IRQ timing (variant 4c) also has hardware evidence against it:
   - Contra 4 clears the Y-trigger bit constantly (the Bakkerrrs hook fired on about 8% of frames).
   - nds-bootstrap blacklists 21 titles for VCOUNT hooks.
5. **New pieces that help:**
   - DSi NDMA can act as a private line timer the game can't see.
   - DISPCNT takes a single-byte write.
   - VRAMCNT is very likely readable (the in-game menu reads it).
   - The camera LED (BPTWL 31h) has a hardware blink and works on 3DS too.
   - The module's measured idle time can gate anything that costs bus time.

## 2. Shape: one library, many presenters

Everything that decides *what* to show and *how it looks* is plain C, has no hardware access, and runs as host tests on a Mac. Only thin presenters touch hardware. Which presenters ship is the owner's decision; the library doesn't depend on it.

```
 module (idle)                         library (dscheevos/display/)                presenters
 dscm_notices_t ring ─┐            ┌─ scheduler: queue, priority, timing ─┐     P0 LEDs (built) + camera LED
 dscm_notices_t indic ├─ ingest ──>│                                       ├──> P1 in-game menu "Now" page (paused, player-asked)
 dscm_list_t ─────────┘            ├─ scene: what is visible this frame    │     P2 top strip (DSi NDMA FIFO), gated
 frame counter, idle slack ──────> │                                       │     P3 black-frame card (same machinery as P2)
 asset reader (SD, safe) ────────> └─ renderer: font + badge -> surface ──┘     P4 bottom-screen text, allow list
                                                                                 P5 3DS TwlBg (later), P6 LAN companion (optional)
```

The scheduler and renderer are the same for every presenter. A presenter reports what it can show this frame (its *capabilities*), and the scheduler fits the scene to that.

## 3. Library API

**Built (host-only, 2026-10-12): `dscheevos/display/`.** `include/dsd.h` is the source of truth; the sketch below was the draft.

| Part | Where | What |
|---|---|---|
| scheduler | `src/dsd_sched.c` | ingest, tick, current item, missed list, visible rows for the in/out animation |
| strip renderer | `src/dsd_render.c` | full (32 rows) and compact (16) layouts; resumable by row band |
| menu "Now" page (P1) | `src/dsd_menu.c` | `dsd_render_now` into the menu's 32x24 tile map |
| font | `tools/fontgen.py` | the in-game menu's 8x8 font and Unicode maps, generated from the nds-bootstrap checkout |
| tests | `tests/test_display.c` | scheduler properties and golden PNGs (`make -C dscheevos/display test`; also in CI) |

Strings are read through a callback (`dsd_text_fn`) when drawing, never copied: rc_client in the module, a table in the tests.

The in/out animation changes the strip's line count (`dsd_visible_rows`), so each item is drawn once. Waiting counts only frames when nothing can be shown, so a burst of unlocks is all shown.

Draft sketch:

No `malloc`: the caller gives all memory, so the module's footprint is visible and fixed. Time is the module's frame counter (VBlanks); the library uses no timers.

```c
/* dscheevos/display/include/dsd.h */
typedef enum { DSD_FMT_RGB555, DSD_FMT_TILE4 } dsd_fmt_t;   /* linear 15-bit, or 4bpp 8x8 tiles + map */
typedef struct { uint16_t w, h, stride; uint8_t fmt; void* pixels; uint16_t* map; } dsd_surface_t;

typedef struct {                        /* what a presenter can do this frame */
  uint8_t  strip_lines;                 /* 0 = no on-screen strip available now (guards failed) */
  uint8_t  card_ok;                     /* black-frame card possible now */
  uint8_t  pinned_ok;                   /* owner setting: may a tracker stay pinned */
} dsd_caps_t;

typedef struct {                        /* one visible element */
  uint8_t  kind;                        /* DSD_POPUP_UNLOCK, ..._PROGRESS, ..._LB, ..._MODE, ..._TRAY */
  uint8_t  phase;                       /* in / hold / out, for 2-3 frame wipes (no alpha on the strip) */
  uint16_t frames_left;
  uint32_t id;                          /* achievement / leaderboard id: badge, and strings via rc_client */
  uint16_t notice_seq;                  /* source notice, for kinds without an rc_client object */
} dsd_item_t;

/* Strings are never copied into the library: titles, descriptions and badge
 * names stay resident in rc_client (memory plan). The renderer asks for them
 * through this callback when it draws; the module implements it with
 * rc_client_get_achievement_info / rc_client_get_leaderboard_info, and falls
 * back to the notice ring's text for kinds without an object. */
typedef const char* (*dsd_text_fn)(void* ctx, uint8_t kind, uint32_t id, uint8_t which /* title, detail */);

typedef struct { uint32_t gen; uint8_t count; dsd_item_t item[4]; uint8_t tray_challenges, tray_trackers; } dsd_scene_t;

typedef struct {                        /* asset reads go through nds-bootstrap's SD path */
  int  (*read_begin)(void* ctx, uint32_t off, void* dst, uint32_t len);  /* queue; never blocks */
  int  (*read_done)(void* ctx);
  void* ctx;
} dsd_io_t;

int  dsd_init(dsd_t* d, void* mem, uint32_t mem_size, const dsd_config_t* cfg, const dsd_io_t* io);
void dsd_ingest(dsd_t* d, const dscm_notices_t* n);                        /* reads ring entries since last seq */
void dsd_tick(dsd_t* d, uint32_t frame, const dsd_caps_t* caps);           /* advance timing, build the scene */
const dsd_scene_t* dsd_scene(const dsd_t* d);
int  dsd_render(dsd_t* d, dsd_surface_t* s, uint32_t budget);             /* resumable; 1 = done */
int  dsd_render_list(dsd_t* d, const dscm_list_t* l, uint32_t first, dsd_surface_t* s); /* menu pages */
```

`dsd_render_list` and `DSD_FMT_TILE4` let the in-game menu (a separate binary) share the renderer, so the "Now" page and the overlay look alike. The library is compiled into both the module and `arm9_igm`.

### Scheduling policy

Durations are in frames at 60 Hz. The strip covers game pixels, so they are shorter than RetroArch's 4 s.

| Notice | Priority | Hold | Rule |
|---|---|---|---|
| Mastery | 1 | 300 | never dropped |
| Unlock | 2 | 180 | queue 8; on overflow, merge into "+N more unlocked" (nothing is silently lost) |
| Leaderboard submitted / failed / rank | 3 | 120 | newest per leaderboard replaces older |
| Leaderboard started | 4 | 120 | dropped if a failed/submitted notice for the same leaderboard is already queued, so "started" never shows after "failed" |
| Session mode (Hardcore / Softcore) | 2 | 180 | first chance after boot, on top of the placard |
| Progress | 5 | 90 | single slot, newest replaces |
| Offline / online | 5 | 120 | |
| Challenge / tracker change | tray | 60 flash | the LED holds the state. A tracker stays pinned only if `pinned_ok` (owner setting, compact 8-12 line strip) |

- **One pop-up on screen at a time.** If no presenter can show it (capabilities 0: a fade, capture in use, too little idle slack), it waits.
- **After 10 s of waiting**, a notice goes to the "missed" list on the menu's Now page and is still cued by LED. RA's evaluation backlog can already delay notices by up to about 17 s; the scheduler stamps each one with its frame and doesn't add to that delay.
- **Torn reads:** ingest copies each ring entry and checks `seq` before and after, and re-reads indicators until `indicators_generation` is stable. The current menu reads the module's indicators with no such check, so the menu should adopt this too.

### Rendering

- **Font:** the in-game menu's 8x8 1bpp font (2 KB, ASCII + one language upper half, already on SD as `ascii.lz77` + language files).
  - A proportional 8 px variant, derived from it at build time, adds about 0.5 KB of width table.
  - Japanese titles beyond the 256-glyph set: render the glyphs the launcher needs into the asset pack per game (the launcher knows the strings).
- **Badges:** the launcher converts to 32x32 4bpp + 16-colour palette, LZ10-compressed (BIOS-decodable). That is about 520 B each, so about 50 KB for a 100-achievement set on SD. The black-frame card can use 64x64 4bpp (about 1.5 KB each) if the owner wants big badges.
- **Strip layouts:**
  - Compact, 16 lines: 16x16 icon + one text line, for progress, trackers and flashes.
  - Full, 32 lines: 32x32 badge + title + points/description, for unlock and mastery.
  - Card: up to 64 lines, centred.
- **Rendering runs in idle time, resumable,** and only when the scene changes, never per frame. A 256x32 RGB555 strip is 8192 pixels; glyph and badge blits are estimated at 30-80k cycles, split into ≤ 10k-cycle slices.

## 4. Where it runs (no new work in the game's IRQ, except where listed)

| Work | Context | Notes |
|---|---|---|
| ingest, tick, render | module idle slice, after the current frame's evaluation, with a fixed cycle budget | Coordinate the slot and budget with the perf session; it owns `dsc_idle` and the halt path |
| asset reads (phase 2 only) | queued from idle, served by an nds-bootstrap SD read path that doesn't exist yet (the perf session designs it) | none in phase 1. SD reads only, never writes. A notice whose badge hasn't arrived waits up to 60 frames, then shows without it |
| P2/P3 arming | module idle, once per notice: start NDMA2 (V-Blank startup + interval) | nothing added to the VBlank IPC hook |
| P2/P3 mode switch | four NDMA-completion IRQs per shown frame (entry ~261, exit N-1, guard N+M-1, re-arm), via the boot-time `OS_IrqHandler` patch, on the module's own IRQ stack | only while a notice is shown; wakes `OS_Halt` (coordinate idle accounting) |
| P4 | VBlank IPC hook, compare-then-write of a handful of registers | Allow-listed games only |
| P0 | ARM7 card engine, outside IRQ where possible | Existing LED code |

### Cost when nothing is shown: ~0
- `dsd_ingest` returns after one compare when `head` and `indicators_generation` haven't changed.
- `dsd_tick` returns at once when the scene is empty and no notice is waiting.
- Nothing renders, no SD read happens and no presenter is armed.

So with no notice pending, the idle cost is two word compares per frame. CPU is the binding constraint for big sets (memory plan), so evaluation always runs first. Render slices run only when the current frame's evaluation is done; a slow render delays a notice, never an evaluation.

## 5. Memory

The perf session owns the memory plan. Its constraints (2026-10-12):
- ≤ 16 KB resident;
- a ≤ 32 KB window only while something is visible;
- no hardcoded addresses: the region is assigned after a fill/write-watch test;
- NWRAM-B/C is the ARM7's (FNT/FAT) until ARM9_SETSCFG, so plan on the main-RAM framebuffer (R7 at ~4.5%) unless the plan finds NWRAM.

Background: Every byte reserved in DSi extra RAM comes out of nds-bootstrap's ROM cache and preloads; Mario Kart DS's longer loads showed this. So the design targets the smallest resident set, and only takes the larger window while something is on screen.

**Phase 1 is text-only on the strip; badges appear only on the paused menu page.** This was revised after the red-team:
- The SD read path from module idle doesn't exist yet. It needs a cluster map, a new ARM7 command, and serialization with CE9 and the SDIO shim.
- Every such read shifts ARM7 timing, the class of change that exposed today's Kirby SD race.

So no SD reads during play until the perf session designs that path. In-play badges are phase 2.

| Piece | Bytes | Where |
|---|---|---|
| Library code (scheduler, renderer) | ~5-7 KB Thumb | module image |
| P2 presenter (arming, 4 NDMA handlers, guards, IRQ trampoline, own IRQ stack) | ~1.5-2.5 KB | module image + bss |
| Scheduler state + scene + queue (ids only; strings read through rc_client) | ~0.5 KB | module bss |
| Font: 256 glyphs, the session's language (ASCII + one upper half) | 2 KB + 0.3 KB width table | module bss, loaded at boot |
| **Resident total, phase 1** | **~9-12 KB** (cap 16 KB) | |
| Strip framebuffer | 16 KB (256x32 RGB555), shown only after a complete render, rendered with the strip down (the window holds one strip; re-rendering while shown would need 32 KB) | **window** from the memory plan, only while a notice is up. Written through the uncached mirror, so the FIFO NDMA reads current data |
| Card framebuffer (P3, later) | 32 KB (256x64) | same window, never at the same time as the strip |
| Phase 2: badge pack | ~50 KB per typical set, ~130 KB heavy | SD, `sd:/_nds/dscheevos/display/<gameid>.pak`, needs the SD read path |
| Phase 2: badge staging + 1 decoded slot | ~1.1 KB | module bss |

- **The window needs a protocol** with the memory plan. If it is carved from the ROM cache, CE9 must not use it while it is lent, and must re-validate its cache slots after.
- **The ABI v6 notice ring and list already copy strings** (about 1.9 KB of notices, 176 B per list entry). That isn't display code, but it counts against the module; it's the perf session's call whether v7 slims it.
- **Glyphs outside the session language's 256-character set** show as `?` in phase 1. Phase 2 lets the launcher pre-render titles that need them into the pack.
- **Minimum footprint** (P0 + P1 only, no strip): about 7-9 KB resident, and no window at all.
- **The window is a 16-32 KB request to the memory plan.** No address is claimed.

Asset pack layout:
- header;
- index (achievement id → offset, length);
- LZ10 badges;
- the per-game glyph extension.

The launcher builds it before boot (it is online then): it downloads `media.retroachievements.org/Badge/<name>.png`, about 5.7 KB each, and caches per game. A first run of a 100-achievement set downloads about 570 KB. This adds no SD writes during play.

## 6. Presenters, ranked

### P0. LEDs (built) + camera LED (new). Rule-clean
- The Wi-Fi LED keeps its unlock (3 blinks) and progress (1) cues.
- New: camera LED BPTWL[31h]: 01h steady while a challenge is primed, 02h hardware blink while a leaderboard runs, 00h otherwise.
- Evidence:
  - GBATEK BPTWL 31h (camera LED: 00h off, 01h on, 02h blink);
  - nds-bootstrap already writes it for its ROM-read LED option (`cardenginei/arm7/source/cardengine.c:712,727`);
  - the 3DS MCU emulates register 31h in DSi mode;
  - it doesn't share a register with the Atheros enable bit (unlike 30h bit 4).
- One I2C write per state change instead of per blink edge. LED work is WORKLIST section 3.
- **Residual:** ARM7 I2C time per write (about 0.2-0.4 ms, not measured). No screen or game state.

### P1. In-game menu "Now" page (paused, player-requested). Rule-clean
Active challenges, trackers, near-complete progress, recent and missed notices, rendered by `dsd_render_list` into the menu's tile surface. It shares the menu's existing save and restore, whose open bugs the perf session is triaging.

### P2. Top strip: engine A Main Memory Display, fed and timed by DSi NDMA (design "4f")
Per shown frame (revised after the red-team):
1. **Timing source, phase-locked in hardware.**
   - NDMA2 uses startup 06h (V-Blank) with a BCNT interval, so it fires at a fixed delay after line 192 (target: line 261).
   - It is armed once per notice, from idle, not per frame from the IPC IRQ. So frames where the IPC IRQ is missing (card loads) don't flicker.
2. **NDMA2 IRQ at line ~261 (entry):**
   - **Window check:** if VCOUNT isn't 259-261, abort this frame (no mode change).
   - Re-check the guards.
   - Reset and arm NDMA0: startup 09h, (N+M)x128 words from the framebuffer.
   - Set an `armed_this_frame` flag, then `STRB` DISPCNT byte 2 → mode 3.
   - Arm NDMA3 as the H-Blank counter (startup 07h, TCNT = **N**: H-Blanks during V-Blank don't count).
3. **NDMA3 IRQ at the end of line N-1 (exit):**
   - `STRB` byte 2 back to mode 1 if it reads 3.
   - **Window check:** if VCOUNT > N, log a late exit; after 2 late exits, stand the strip down for this game for the session.
4. **Guard IRQ at line N+M-1 (new, fixes the red-team's B1):**
   - The H-Blank counter is re-armed for M more lines.
   - If byte 2 reads 3 again, a game read-modify-write spanning our exit IRQ wrote it back. Put back 1, and count it.
   - The game's next lines are safe; the cost is a thicker banner border, not garbage.
5. **Next VBlank:**
   - Restore 1 only if `armed_this_frame` is set and the mode still reads 3. A game's own legitimate mode 3 is never touched.
   - Clear the flag.

The game's VCOUNT, H-Blank, timers, VRAM, OAM and palettes are never touched.

**Stand down during card reads:** no strip on frames where CE9 has a card read in flight (`readOngoing`). CE9 uses 128-word physical blocks that GBATEK says are never split (about one line of bus), so they would starve the FIFO, and an underrun shows garbage. This replaces the earlier idea of moving CE9 to NDMA1. The strip simply waits for a frame without card reads.

**Requires the dispatcher patch.** If no `OS_IrqHandler` signature is found, or the game installs its own IRQ vector, P2 is off for that game. Otherwise an unacknowledged NDMA IRQ could crash or hang it. The alternative is a BIOS-vector wrapper (DTCM+3FFC) instead of per-SDK signatures. It is simpler, but it is a write to game DTCM. The owner must choose; see R4.

**Guards (all must pass, else capabilities strip_lines = 0):**
- DISPCNT A mode = 1;
- capture not using FIFO;
- no legacy DMA in mode 4;
- MASTER_BRIGHT A factor < 12;
- display stable for 30 frames;
- **idle slack ≥ the strip's worst-case bus cost in each of the last 8 frames** (excluding frames where idle time was a card wait);
- no card read in flight;
- the dispatcher patch was applied for this game;
- the title is on the allow list.

**Residuals, each needing the owner's ruling** (revised after the red-team; under the project's rules each is a blocker until accepted):

| # | Residual | Size | Mitigation |
|---|---|---|---|
| R1 | DISPCNT A byte 2 reads mode 3 instead of 1 | ~N+2 lines per shown frame (line 261 → N-1), plus up to M lines if a game RMW spans our exit IRQ | late entry; the guard IRQ at N+M-1 catches a write-back. An absolute game write drops the strip for that frame |
| R2 | Game code that saves DISPCNT (`GX_DispOff`, or any whole-register save, inlined or not) can store mode 3 in game RAM if it runs in that window | rare; the game would later turn on mode 3 with no data → garbage until its next DISPCNT write | R3 for the SDK path; the M1 survey logs DISPCNT *reads* in the window per title, and titles with reads there stay off the allow list; detect `GXi_DispMode` = 3 and stand down |
| R3 | Boot-time code patch of `GX_DispOff` (save 1 when it reads 3 while the strip is live) | one SDK function, by signature; may miss inlined/overlay copies | same class as the existing `OS_Halt` patch |
| R4 | Boot-time code patch of the game's `OS_IrqHandler` so IF bits 28-31 reach the module | one SDK function per SDK version, by signature | **Precedent corrected:** the existing IPC hook writes the game's `OS_IRQTable` in DTCM at runtime; it isn't a code patch. The closest code-patch precedent is `OS_Halt`. Alternative: a BIOS-vector wrapper (a write into game DTCM) |
| R5 | SCFG_EXT9 bit 8 set before the SCFG lock | NTR games can now *see and set* IE bits 14/15 and 24-31; DSi-mode games already run with it | test on hardware (H7) |
| R15 | IE bits 28-31 set by us are visible to a game reading IE; a game writing IE absolutely clears them | readable; on a clear, our exit IRQ is lost → garbage for the rest of that frame | extend nds-bootstrap's IE hook to re-OR bits 28-31 on every game IE write, not only on card reads (`misc.c:128-134`); the guard IRQ is lost too in that case, so the M1 survey's absolute IE writes gate the allow list |
| R16 | Our exit IRQ can delay a game's H-Blank raster or HUD-split write from line N-1 into line N | one game line torn, on frames where the game does mid-frame raster writes near line N | M1 logs raster IRQs per title; titles with H-Blank/VCOUNT work within ±2 lines of N are off the allow list or get a different N |
| R7 | Bus time while a notice is shown | ~3.2% of the frame (main RAM framebuffer, contends with ARM7 audio); ~1.8% only if NWRAM becomes available | the idle-slack gate **lowers the odds of a lag frame; it doesn't prevent one**. Unlocks can coincide with load spikes, and the ARM7 isn't gated. The owner rules on "rare lag frame possible while a notice shows" |
| R8 | IRQ entries; NDMA IRQs also wake `OS_Halt` | 4 per shown frame, ~0.15-0.25% of a frame; extra idle passes skew the module's idle stats | short handlers on the module's own IRQ stack; coordinate idle accounting with the perf session |
| R9 | The notice hides the game's top N lines | 16-32 lines for 1.5-5 s; opaque | short holds; deferred during fades; pinned tracker only by owner setting |
| R10 | Master brightness fades the strip with the game | cosmetic | deferred while the factor is ≥ 12 |

The earlier R6 (moving CE9 to NDMA1) is dropped: the strip stands down during card reads instead.

### P3. Black-frame card
Full-screen notices with 64x64 badges, shown on frames where engine A outputs only a uniform colour (forced blank, all layers off, display mode 0): scene changes and loads. It uses P2's machinery and residuals, and needs R3. Extra residual **R11**: FIFO bus time for 64-96 lines (about 10-30% of bus) on load frames, which could lengthen a load slightly; card-bound loads may not be idle-gated. It is only worth building if the M3 census finds black stretches common.

### P4. Bottom-screen text (fallback tier 1)
Only when engine B shows the backdrop only (DISPCNT_B bits 8-12 clear) for 30 frames **and** bank H or I reads back disabled.
1. Map that bank as sub BG, point one sub BGCNT at it, write 16 palette entries, set one layer bit.
2. Every VBlank, compare each of our words with the live value. If the game changed any, drop it and end the notice within a frame.
3. On exit, restore only words that still hold our value. **Compare and restore per bit, not per word**, for shared registers such as DISPCNT_B. Otherwise a game write to another bit in the same word would make us skip restoring our layer bit, leaving it on.

**Residuals:**
- **R12:** game registers (BGCNT, a DISPCNT_B bit, VRAMCNT H/I) and 16 palette entries hold our values for the notice's length.
- **R13:** a 1-frame overdraw if the game wakes the sub screen during a notice.
- **R14:** writes into a VRAM bank the game has disabled (undefined contents to the game).

Allow list only.

### P0 alone is fallback tier 2
LED cues plus the menu, if the owner rejects every on-screen residual.

### Later: P5 3DS TwlBg (the real answer on 3DS: no DS-side residual), P6 LAN companion (UDP per notice; zero game cost)

### Rejected (with reasons in `display-research/hardware.md`)
- takeover pop-ups;
- sprite/BG borrowing as a default (per-title trace-built opt-in at most);
- capture compositing (a frame of latency plus a bank);
- master brightness, windows, blending (game-owned, some write-only);
- 3D rear plane;
- LCD swap;
- the backlight cue (nonvolatile MCU register);
- the VCOUNT-IRQ strip (4c) as primary timing;
- the NDMA-only switch-back (4a, clobbers DISPCNT).

## 7. Test plan

Pass/fail marks are fixed here, before any run. Results go in TESTING.md. No emulator run without the perf session's go.

**Host (no emulator, any time):**
- **D1. Scheduler properties:**
  - every ingested notice is shown, merged or listed as missed (never lost);
  - priority order;
  - holds within ±1 frame;
  - torn-ring reads rejected.
- **D2. Golden PNGs** for every layout × notice kind × UTF-8 edge case (long titles, accents, Japanese); review by eye once, then byte-compare.
- **D3. Static signature census** over the RA-set ROM dumps (`private/roms`, RomM): `OS_IrqHandler` and `GX_DispOff` found in ≥ 90% of titles (M2). A ROM scan only; no emulator.
- **D4. Asset pack:** the launcher-side converter round-trips: the decoded badge matches the reference within the palette error, and the pack index is valid.

**melonDS (queued, coordinated):**
- **M1. Ownership survey** per RA-set title (DISPCNT writes with VCOUNT, capture, DMA mode 4, DISPSTAT/IE absolute writes, VRAMCNT, MASTER_BRIGHT, engine-B layers, `GX_DispOff` calls). **Pass for P2:** ≥ 80% of titles are compatible; < 50% stops P2.
- **M3.** Black-stretch census: median title ≥ 1 stretch of ≥ 30 frames per 5 min, or don't build P3.
- **M5.** Idle slack: ≥ 5% idle in ≥ 90% of play frames in ≥ 80% of titles, or the gate would rarely open.
- **M6.** Patch the melonDS fork to gate IE 28-31 on SCFG_EXT9 bit 8 and model NDMA intervals. **Pass:** the old `overlay-fifo` S9 scenario fails without bit 8 and passes with it.
- **M7.** End-to-end P2 with Kirby, NSMB, Pokémon Platinum and Contra 4, a forced notice every 5 s for 10 min. **Pass:**
  - 0 pixel differences below line **N** (the margin lines must match the game too, or the border is counted as a failure);
  - identical lag count (same input script, deterministic emulator);
  - late-exit and guard-IRQ counters both 0;
  - `GXi_DispMode` never reads 3.

**DSi XL** (standalone test ROMs first; then the real chain). Each step is a hardware log entry with the pass mark above it:
- **H1.** NDMA 09h feeds the FIFO (banner on lines 0-15, the game pattern from 16-18, photographed).
- **H2.** An NDMA BCNT one-shot fires within ±1 line, 1000 trials × 3 delays.
- **H3.** A byte-2 `STRB` switch gives a clean edge in 600 frames; bytes 0, 1 and 3 are unchanged.
- **H4.** VRAMCNT A-I read back equal to what was written.
- **H5.** NWRAM works as the FIFO source; bus cost < 0.5% of a frame (threshold for the owner).
- **H6.** Starvation: record banner errors with the FIFO on NDMA0 under card NDMA (128-word blocks) and a legacy DMA3 burst. **Pass:** none with legacy DMA3 alone. With card NDMA, errors are *expected*, which confirms the stand-down rule; record what they look like.
- **H6b.** Entry/exit IRQ latency: log VCOUNT at entry and exit in both handlers over 10,000 frames of a busy test ROM (IRQs-off sections up to 2 lines). **Pass:** choose M as the 99.99th-percentile exit lateness + 1 line, and M ≤ 4.
- **H7.** Real chain, 4 games × 30 min:
  - no crash;
  - lag statistically equal to stock: 3 off/on run pairs of 30 min each, scripted input from the lab rig, or the owner's own play, with the difference within the off/off spread;
  - no difference outside the strip on 240 fps video;
  - 3 module IRQs on every shown frame.
- **H8.** Arming at line 261 starts the banner at line 0 in 600 frames.
- **H9.** Camera LED responds and blinks:
  - I2C write < 0.5 ms;
  - the Atheros stays associated for 10 min;
  - no audio glitch;
  - also on the New 3DS XL.

## 8. Build order

1. Library + host tests (D1, D2) on `display`. No module change. Phase 1 is text-only (no asset pack, so no D4 yet).
2. D3 signature census (static).
3. Standalone test ROMs for H1-H6b and H8 → the owner runs them on the DSi XL.
4. Coordinated melonDS M6 patch, then M1, M3, M5.
5. If the owner accepts the residuals and H1-H8 pass:
   - integrate P2 behind an allow list and the idle-slack gate;
   - send the design to the perf session before any patch to game code;
   - run M7, then H7.
6. P1 Now page. Camera LED (WORKLIST section 3).

## 9. Red-team outcome (2026-10-12)

Full report: `docs/display-research/red-team.md`. All three blockers are addressed in the design above. Whether the fixes hold is decided by H6b, M7 and H7.

| Finding | Severity | Change |
|---|---|---|
| B1: a game DISPCNT read-modify-write spanning our exit IRQ writes mode 3 back, so garbage to the end of the frame | blocker | guard IRQ at line N+M-1; M1 logs DISPCNT reads in the window; R1 widened |
| B2: no measured IRQ latency, no abort rule; CE9's 128-word blocks starve the FIFO | blocker | VCOUNT window checks with abort and stand-down; M from H6b; no strip while a card read is in flight |
| B3: a missed dispatcher patch or game vector at DTCM+3FFC → crash or IRQ storm; IE bits visible; lazy IE re-enable | blocker | P2 requires a patch hit; R15 added; IE hook re-ORs on every game IE write |
| VBlank fixer clobbered a game's own mode 3 | major | restore only if `armed_this_frame` |
| Arming from the IPC IRQ flickers on load frames | major | NDMA startup 06h, armed once per notice |
| TCNT off by one | major | TCNT = N |
| Our exit IRQ can tear a game raster line | major | R16 + allow-list rule |
| Pass marks hid failures | major | M7 below N; H7 statistical |
| The slack gate isn't a guarantee | major | R7 reworded; the owner rules |
| Memory tally incomplete; no window protocol; no cache coherence | major | tally redone; uncached writes; window protocol is an ask to the memory plan |
| The SD asset path doesn't exist | major | phase 1 text-only, badges on the paused menu page only |
| The `GX_DispOff` patch is too narrow | major | R2 widened; M1 read logging |
| R4 cited the wrong precedent | major | corrected |
| P4 restore per word | major | per bit |
| Bus 3.2% not 4.5%; SCFG bit 8 also exposes IE 14/15/24-27; NDMA0 reset; `ndmaBusy(0)`; own IRQ stack; OS_Halt wakes; started-after-failed | minor | folded in (R5, R7, R8, P2 steps, scheduler) |
| Simpler: launcher pre-renders notice bitmaps | alternative | kept for phase 2 (it removes the font and the CJK problem, but needs the SD path) |
| P2 not needed on 3DS | minor | agreed: 3DS uses P5 |

## 10. Decisions for the owner

1. Rule on each residual (R1-R5, R7-R16; R6 was dropped) (accept / reject). Rejecting R1 or R4 rules out every on-screen DSi option except P4.
2. Strip height (16 compact / 32 full) and whether a leaderboard tracker may stay pinned.
3. Build P3 (black-frame card) if M3 passes?
4. Want P4 (bottom-screen text, allow list) as a fallback?
5. Phase 2: badge download at the launcher, about 570 KB per 100-achievement set, cached on SD. OK?
6. Camera LED for challenges and leaderboards now?
7. R7: is a *rare* lag frame acceptable while a notice is on screen (the slack gate lowers the odds but can't guarantee zero)? If not, P2 is out and P4/P0 remain.
8. R4: patch `OS_IrqHandler` per SDK version (a code patch), or wrap the BIOS IRQ vector (one write to game DTCM)?
