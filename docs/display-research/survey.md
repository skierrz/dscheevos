# M1 game survey: what DS games do with the registers P2 touches

Measured 2026-10-12 in melonDS (headless, plain DS mode). This gives the owner data for the P2 residuals R1, R2, R15 and R16 (`display-library.md` section 6), for M1, M2 and M3 (section 7), and for `hardware.md` section 4. Strip geometry assumed throughout: N = 32 lines, margin M = 4.

## Headline

| Mark | Pass line | Result | Reading |
|---|---|---|---|
| **M1** (P2 allow list) | >= 80% compatible: build; < 50%: stop | Rules as set: **18 of 27** titles that reached play (67%); 18 of 30 with a known verdict (60%); 18 of 45 overall. Lenient reading: **20 of 27** (74%) | **Between the marks: the owner's call.** No title uses FIFO capture or DMA mode 4. The failures are raster work at lines 30-38 (6 titles) and DISPCNT A writes in the window (5 titles). Only 2 titles change the *mode* there. |
| **M2** (`OS_IrqHandler` signatures) | >= 90% covered | **4 handler variants** in 45 titles. The top variant covers 36 (80%); the top two cover 43 (96%) | Pass for `OS_IrqHandler` with 2 signatures (3 for 98%). `GX_DispOff` wasn't censused here (D3 is a static ROM scan) |
| **M3** (black-frame card, P3) | median title >= 1 black stretch (>= 30 frames) per 5 min | Median **0.0** per 5 min (all 45, and the 27 that reached play). Counting full-brightness fades (factor 16) as black: median 0.67 (all), 2.0 (the 27 that reached play) | **Fail** under the definition as written. It would pass only if fade-to-black/white counts as a black frame *and* only gameplay titles count. Owner's call on the definition |
| R15 (absolute IE writes) | | Every title does one at boot (crt0/`OS_InitIrqTable`, frames 4-30, before any strip could be armed). After boot, only **Zelda: Phantom Hourglass** (4 in 15 min, PC `0200c3dc`, in the Nintendo WFC setup screens) | R15 is nearly empty after boot. Extending the IE hook (R15's mitigation) covers the one case |
| R16 (raster near N, N+M) | | 6 titles write display registers from IRQs (or line-locked code) at lines 30-38. 8 titles have the HBlank IRQ on every line of every frame | Every-line HBlank users fail for **any** N; choosing a different N doesn't help them |
| R2 (DISPCNT reads in the window) | | Common: 34 titles read byte 2 in the window while live, and 30 do so without an RMW write after it (bare). Disassembly shows nearly all are SDK getters that mask other fields (BG/OBJ base, BG mode). 5 titles have sites whose code may keep bits 16-17 (Pokemon Black, Trauma Center 2, Phantom Hourglass, MvDK, M&L BIS; section 4.3). No write that sets mode 0 (the `GX_DispOff` pattern) ran in the window while live in any title | As written, "titles with reads there stay off the allow list" would remove 34 of 45 titles, so the reads are reported and don't gate. R3 (patch `GX_DispOff`) or the `GXi_DispMode` check is what really covers R2 |

## 1. Method

**Emulator.** This is melonDS, the `dscheevos-perf` fork on branch `dscheevos-display-survey` (worktree `~/Developer/DScheevos/melonDS-survey`), interpreter only, no JIT. It runs in DS mode with the real `biosnds7/9.bin` and `dsfirmware.bin`. There is no nds-bootstrap and no DScheevos module: this measures the games' own behaviour. ROMs are direct-booted. The RTC is the host clock.

**Hooks** (commits `e85ddf01`, `18d00053` and `b01e8126`; off unless `--survey PREFIX` is given):
- `NDS::IOHook`: every ARM9 bus access, CPU or DMA, to `0x04000000-0x04001FFF`. It is called with the value for reads, and before and after each write. Each access is tagged with the CPU mode, or the DMA channel when a legacy DMA is running.
- `NDS::LineHook`: called at the end of `GPU::StartScanline` with the new VCOUNT.
- The runner (`tools/dscheevos/headless/survey.cpp`) keeps aggregates and writes two files:
  - `PREFIX.json`: aggregates;
  - `PREFIX.frames.csv`: one row per frame, sampled at line 0.

**What is logged:**
1. **DISPCNT A and B:** every access (8/16/32-bit), keyed by PC, CPU mode, size and offset.
   - Line buckets: 0-35 (strip and margin), 36-191, 192-260 (VBlank) and 261-262 (strip entry).
   - Writes that cover byte 2 (bits 16-23, the mode byte) are classed as:
     - **pure RMW:** a byte-2 read since the previous write, within 2 lines, and the mode bits unchanged;
     - **RMW-mode:** read, then the mode changed (`GX_DispOff`'s pattern);
     - **absolute:** no read before it.
   - Writes that don't cover byte 2 can't touch the mode, so they're harmless.
   - The **window** is lines 261-262 and 0-35.
   - **"While live":** engine A was in mode 1 at line 0 for the last 30 frames, P2's own stability guard. This keeps out the boot-time `GX_Init` writes (old = new = 0), which every title does once.
   - A **bare read** is a byte-2 read of DISPCNT A not followed by an RMW write within 2 lines (the value went elsewhere: R2). The code around each bare-read PC is saved for disassembly.
2. **IE (R15):** a phantom bit.
   - At boot the runner sets ARM9 IE bit 28 internally. Nothing raises IF bit 28 in DS mode, so it never fires.
   - After every game write to `0x04000210-213`, if bit 28 is gone, it was an absolute write. PC, line and value are logged, and the bit is put back.
   - melonDS check: `NDS::ARM9IOWrite32` stores `IE[0] = val` unmasked (`NDS.cpp:3551`), and the 16-bit path likewise. Nothing in `DSi.cpp` gates IE/IF bits 28-31 on SCFG_EXT9 bit 8 (the `SCFG_EXT[0]` write keeps bit 8 but nothing reads it for IRQs). In DS mode melonDS also lets a game read and write IE bits 22-31, which real NTR hardware probably doesn't. That is what makes the phantom bit visible to the game's RMW code; on hardware our bits would only be visible with SCFG_EXT9 bit 8 set (R5). melonDS has no 8-bit IE write path (`ARM9IOWrite8` has no case for `0x210-0x213`). A game's STRB to IE would be dropped silently and not seen here; none was logged as an unknown write.
3. **Raster (R16):**
   - Per line: HBlank IRQ armed (IME, IE bit 1 and DISPSTAT bit 4), and VCOUNT IRQ fired (IE bit 2, DISPSTAT bit 5 and VCOUNT = LYC).
   - Every write to `0x04000000-6F` / `0x04001000-6F`, keyed by (register, PC, mode or DMA channel), with a 263-line histogram.
   - A key counts as **raster** if it is written:
     - in CPU IRQ mode during lines 0-191; or
     - line-locked: >= 30 frames and >= 80% of its writes within one 3-line span of 0-191.
   - It is an **R16 hit** if it has >= 5 writes at lines 30-38 (N±2 and N+M±2).
   - DMA keys written on >= 30 frames during 0-191 are listed separately as HBlank-DMA raster. Our exit IRQ doesn't delay DMA.
4. **Capture / FIFO / DMA mode 4:**
   - DISPCAPCNT after each write, when enable is set: FIFO = source B is main memory and the source select isn't A-only;
   - CPU or DMA writes to `0x04000068`;
   - ARM9 DMA control writes with enable and start mode 4.
   - MASTER_BRIGHT A at line 0 of each frame: fade frames are mode up/down with factor >= 12.
5. **Black frames** (P3), at line 0: engine A forced blank (bit 7), or display mode 0, or layers 8-12 all off.
   - Stretches of >= 30 such frames are counted and scaled to 5 minutes. A stretch starting at frame 0 (boot) isn't counted.
   - A second count adds full fades (factor 16, up or down).
6. **IRQ handler (R4/M2):**
   - At frames 600, 3600, 18000 and at exit, the runner reads the pointer at DTCM+`0x3FFC` (DTCM base from melonDS's CP15 state) and dumps the 0x80 bytes there.
   - Variants are hashed on **the handler's own code**: the bytes up to its literal pool, i.e. the lowest PC-relative LDR target in the dump.
   - Raw 0x80-byte hashes are also reported. They mostly split on whatever function the linker placed next.
   - In every title the pointer was the same at all four samples.

**Input.** The script is generated by `make_script.py`, is deterministic and is the same for every title.
- First 90 s: START, A and a centre tap in turn every 20 frames, with an occasional DOWN.
- Then: d-pad holds, A/B/X/Y presses and taps at 8 spots, with START+A+START every ~20 s.
- Titles that hadn't reached play got a second run with `--touchy`: all-run menu mashing with taps over a 4x4 grid plus common button spots. That run replaces the generic one in the tables (marked "(t)").
- Each title ran **15 emulated minutes (54,000 frames)**, one run. Screenshots at 5, 10 and 15 min were checked by eye.
- "Reached gameplay" = at least one screenshot shows the game proper: a level, overworld, trial, puzzle or race. Menus, name entry, tutorials-before-play and title screens don't count. "Partial" = in-game but not core play (a drawing editor, a recipe menu, a hub).

**Titles.** 45 in all:
- the 35 RomM dumps with an RA set (`private/sets/gameids.tsv`, non-zero ID);
- 10 more without a set, for breadth: SM64DS, Yoshi's Island DS, M&L Bowser's Inside Story, Super Princess Peach, WarioWare Touched!, Kirby Canvas Curse, Mario Party DS, Diddy Kong Racing DS, Metroid Prime Pinball, LEGO Star Wars TCS.

**Scripts** (in `tests/display-survey/`, not committed):
- `make_script.py`: input;
- `run_survey.sh`: one title per `emu-lock` hold; needs `ROMDIR`, takes `DSI_FILES` from the env file, and the runner from `SURVEY_RUNNER` or the default;
- `summarize.py`: the tables here; needs `capstone` for handler variants;
- `bare_reads.py`: bare-read disassembly.

Raw logs and IRQ dumps are in the session scratchpad and aren't committed (they contain ROM bytes).

**Reproduce:** `ROMDIR=... run_survey.sh OUT "Title.nds"...`, then `summarize.py OUT [OUT_touchy] --gameplay gameplay.tsv --dumps DUMPS`.

## 2. melonDS accuracy limits that matter here

- **Rendering and register latching are per line.** A register write takes effect at the next line's draw, with no mid-line timing. Line numbers for accesses are the VCOUNT at the time of the access, accurate to the line, not to the dot.
- **No bus contention model for the ARM9 against the display, and cache/TCM timing is flat.** So *when* in a frame a game's code runs is approximate: a write that lands at line 31 here might land at line 29 or 33 on hardware. The R16 band (±2 lines) and the 2-line RMW window absorb some of this, but not all.
- **IRQ latency is idealised.** IRQ-mode raster writes happen on the line their IRQ fires, so the R16 histogram is if anything tighter than hardware.
- **DS mode, not DSi mode.** These games would run in DS mode under nds-bootstrap on a DSi too (NTR titles), but:
  - DSi-enhanced titles (Pokemon Black, MvDK Mini-Land Mayhem) were measured in their DS code path;
  - SCFG_EXT9 bit 8 isn't modelled (above), so the IE phantom bit is visible to games here, and hardware may differ (R5).
- **8-bit IE writes are dropped by melonDS.** A game that uses them would be mis-measured. None was seen through the 16/32-bit paths, but an STRB would be invisible.
- **The phantom IE bit changes what a game reads from IE.** A game that compares IE against a constant would behave differently. None was seen to (all 45 ran normally), but it isn't zero-impact.
- **One run per title with scripted input**, not human play. Coverage of a game's modes is partial: menus, the first stage or scene, sometimes one mini-game. Behaviour in later levels (cutscene FMV, boss raster effects, multiplayer, Wi-Fi) isn't covered.
- **Event lists are capped**: the first 100 window reads, 300 window writes, 300 live non-RMW writes, 200 mode changes and 300 IE events per title. Counts are never capped; only the per-event detail is. One cap-dependent column is the "mode-changing" count in the lenient verdict, taken from the live-write list. No title came near 300 live non-RMW writes (the most was 114).
- **"Raster" is a heuristic.** IRQ-mode writes are certain. "Line-locked" catches VCOUNT polling loops and SYS-mode handlers, but can also flag main-loop code that happens to land on the same lines every frame (Cory in the House's BLDCNT/BLDALPHA writes at PC `0200409c` look like that: 42 writes in 15 min).
- **Bare-read classification is static**, over 10 instructions after the load. Masks loaded from a literal pool (common in Thumb) show as "unclear". Code dumped at exit may have been replaced by an overlay since the read.
- **Screenshot-based "gameplay"** is a human reading of 3 frames per title.

## 3. Per-title results

Columns:
- **Live frames %**: frames where P2's mode-1 stability guard would pass.
- **DISPCNT A w / r per frame**: all accesses, whole run.
- **A window writes while live**: byte-2 writes in lines 261-262/0-35 while live, pure RMW / not.
- **A byte-2 reads in window while live**: all such reads, with bare reads (not followed by an RMW write) in brackets.
- **A mode change outside VBlank**: whole run, boot included (the `GX_Init` 0 → 1 at line 0 is the usual 1).
- **Abs. IE writes**: boot = first 120 frames.
- **HBlank / VCOUNT IRQ frames %**: share of frames with the ARM9 HBlank IRQ armed on any line, or a VCOUNT IRQ fired. VCOUNT lines are the most frequent match lines.
- **Raster keys (R16 band hits)**: see method item 3.
- **Capture en / FIFO / DMA4**: capture-enabling writes / FIFO uses / DMA mode 4 starts.
- **Fade >= 12 %**: frames with engine A master brightness factor >= 12.
- **Black >= 30f per 5 min**: strict definition, with fade 16 included in brackets.
- **IRQ handler**: variant, see section 5.
- "(t)": the touch-script run.

| Title | Gameplay | Live frames % | DISPCNT A w / r per frame | A window writes while live: RMW / non-RMW | A byte-2 reads in window while live (bare) | A mode change outside VBlank | Abs. IE writes boot / later | HBlank IRQ frames % | VCOUNT IRQ frames % (lines) | Raster keys (R16 band hits) | HBlank-DMA keys | Capture en / FIFO / DMA4 | Fade>=12 % | Black >=30f per 5 min (incl. fade 16) | IRQ handler |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Animal Crossing - Wild World | yes | 100 | 3.6 / 4.2 | 18129 / 0 | 24964 (6835) | 1 | 1 / 0 | 100 | 0 () | 14 (13) | 0 | 0 / 0 / 0 | 0.0 | 0.0 (0.0) | V1 |
| Clubhouse Games (t) | yes | 100 | 2.0 / 2.0 | 0 / 0 | 0 (0) | 1 | 1 / 0 | 0 | 100 (191) | 4 (0) | 0 | 53891 / 0 / 0 | 0.0 | 0.0 (0.0) | V1 |
| Cooking Mama | partial | 100 | 0.0 / 0.0 | 0 / 0 | 0 (0) | 0 | 2 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.3 | 0.0 (0.0) | V1 |
| Cory in the House | yes | 100 | 0.0 / 0.0 | 16 / 0 | 130 (114) | 0 | 1 / 0 | 0 | 0 () | 2 (2) | 0 | 0 / 0 / 0 | 0.8 | 0.0 (2.0) | V1 |
| Diddy Kong Racing DS | yes | 100 | 0.0 / 0.0 | 11 / 0 | 11 (0) | 1 | 1 / 0 | 1 | 0 () | 18 (16) | 0 | 1542 / 0 / 0 | 0.0 | 0.0 (0.0) | V3 |
| Digimon World DS | yes | 100 | 0.0 / 1.9 | 21 / 0 | 21 (0) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.0 | 0.0 (0.0) | V1 |
| Drawn to Life | partial | 100 | 0.0 / 0.0 | 3 / 0 | 18 (15) | 1 | 2 / 0 | 0 | 0 () | 1 (1) | 0 | 0 / 0 / 0 | 2.5 | 0.0 (3.3) | V1 |
| Ghost Trick - Phantom Detective | yes | 100 | 0.0 / 0.0 | 0 / 0 | 0 (0) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 1 / 0 / 0 | 0.5 | 0.0 (1.0) | V1 |
| Golden Sun - Dark Dawn | yes | 55 | 0.3 / 0.8 | 6 / 1 | 78 (71) | 4 | 0 / 0 | 0 | 0 () | 0 (0) | 0 | 28296 / 0 / 0 | 13.3 | 0.0 (2.7) | V4 |
| Kirby - Canvas Curse | no | 100 | 2.0 / 0.0 | 0 / 0 | 0 (0) | 0 | 1 / 0 | 100 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.1 | 0.0 (0.0) | V1 |
| Kirby - Squeak Squad | yes | 100 | 0.2 / 4.6 | 0 / 0 | 212 (212) | 0 | 2 / 0 | 0 | 0 () | 4 (0) | 0 | 0 / 0 / 0 | 0.0 | 0.3 (0.3) | V1 |
| Kirby Super Star Ultra | yes | 100 | 2.0 / 1.9 | 0 / 2 | 0 (0) | 0 | 1 / 0 | 100 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 4.5 | 3.0 (3.0) | V1 |
| Legend of Zelda, The - Phantom Hou (t) | no | 100 | 0.0 / 0.2 | 49 / 0 | 281 (232) | 3 | 1 / 4 | 0 | 0 () | 16 (0) | 0 | 196 / 0 / 0 | 0.7 | 0.3 (1.7) | V1 |
| Legend of Zelda, The - Spirit Trac (t) | no | 100 | 1.0 / 4.3 | 4 / 0 | 1553 (1549) | 1 | 1 / 0 | 0 | 0 () | 2 (0) | 0 | 369 / 0 / 0 | 0.4 | 0.0 (0.7) | V1 |
| Legendary Starfy, The | yes | 99 | 0.5 / 2.0 | 97 / 0 | 227 (130) | 2 | 1 / 0 | 64 | 0 () | 7 (2) | 71 | 0 / 0 / 0 | 16.7 | 0.0 (51.0) | V1 |
| LEGO Star Wars - The Complete Saga | partial | 100 | 0.9 / 2.2 | 0 / 0 | 2064 (2064) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 51254 / 0 / 0 | 0.5 | 0.0 (0.7) | V1 |
| Mario & Luigi - Bowser's Inside St | yes | 100 | 0.0 / 0.3 | 30 / 0 | 261 (231) | 1 | 1 / 0 | 0 | 64 (192,168,191,128) | 7 (0) | 1 | 91 / 0 / 0 | 10.3 | 1.3 (9.0) | V2 |
| Mario Kart DS | yes | 100 | 0.8 / 1.6 | 30 / 0 | 50 (20) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 262 / 0 / 0 | 5.2 | 0.0 (8.3) | V1 |
| Mario Party DS | no | 100 | 1.0 / 0.0 | 24 / 114 | 24 (0) | 0 | 1 / 0 | 100 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 6.7 | 4.7 (7.3) | V1 |
| Mario vs. Donkey Kong - Mini-Land  (t) | no | 100 | 0.9 / 1.0 | 2 / 0 | 168 (166) | 0 | 1 / 0 | 0 | 0 () | 11 (0) | 0 | 0 / 0 / 0 | 11.5 | 0.0 (26.3) | V1 |
| Metroid Prime Pinball | yes | 100 | 1.0 / 1.2 | 34 / 0 | 55 (21) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.0 | 0.0 (0.0) | V1 |
| New Super Mario Bros. | yes | 100 | 0.0 / 13.6 | 30 / 0 | 4098 (4068) | 1 | 1 / 0 | 0 | 0 () | 0 (0) | 2 | 1 / 0 / 0 | 0.0 | 4.7 (4.7) | V1 |
| Nine Hours, Nine Persons, Nine Doo | yes | 92 | 2.7 / 2.8 | 0 / 0 | 60 (60) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.0 | 0.0 (0.0) | V2 |
| Nintendogs - Best Friends (t) | no | 100 | 1.0 / 1.0 | 0 / 0 | 0 (0) | 0 | 1 / 0 | 100 | 0 () | 0 (0) | 0 | 179 / 0 / 0 | 0.3 | 0.0 (0.7) | V1 |
| Pac'n Roll (t) | no | 100 | 0.0 / 1.0 | 0 / 0 | 0 (0) | 1 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 53749 / 0 / 0 | 0.0 | 0.0 (0.0) | V1 |
| Peggle Dual Shot (t) | yes | 100 | 1.9 / 1.9 | 0 / 0 | 0 (0) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 51624 / 0 / 0 | 0.1 | 0.0 (0.3) | V2 |
| Phoenix Wright - Ace Attorney | yes | 100 | 1.0 / 0.0 | 0 / 0 | 4 (4) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.0 | 0.0 (0.0) | V1 |
| Plants vs. Zombies (t) | no | 100 | 0.0 / 0.0 | 0 / 0 | 7 (7) | 1 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.5 | 0.0 (0.3) | V1 |
| Pokemon - Black Version | yes | 99 | 0.7 / 1.1 | 286 / 0 | 881 (595) | 1 | 0 / 0 | 100 | 0 () | 8 (8) | 0 | 170 / 0 / 0 | 12.2 | 0.3 (27.3) | V1 |
| Pokemon - Platinum Version (t) | no | 100 | 0.0 / 0.5 | 6 / 0 | 12 (6) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.4 | 0.0 (0.7) | V2 |
| Pokemon - SoulSilver Version (t) | no | 100 | 0.0 / 0.1 | 2 / 0 | 771 (769) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 1.5 | 0.0 (0.3) | V1 |
| Professor Layton and the Curious V (t) | yes | 97 | 0.0 / 1.0 | 2 / 0 | 2 (0) | 2 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 1.5 | 0.0 (1.3) | V2 |
| Resident Evil - Deadly Silence | yes | 100 | 0.0 / 1.2 | 260 / 0 | 835 (575) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 19 / 0 / 0 | 7.6 | 0.0 (17.3) | V1 |
| Rhythm Heaven | no | 100 | 1.0 / 1.0 | 1 / 0 | 3 (2) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.0 | 0.0 (0.0) | V2 |
| Sonic Colors | yes | 99 | 1.2 / 1.4 | 52 / 1 | 128 (75) | 3 | 1 / 0 | 0 | 0 (262) | 3 (0) | 24 | 10 / 0 / 0 | 2.1 | 0.0 (2.0) | V1 |
| Sonic Rush | yes | 100 | 1.0 / 1.0 | 13 / 0 | 20 (7) | 1 | 1 / 0 | 0 | 0 () | 0 (0) | 1 | 0 / 0 / 0 | 0.6 | 0.0 (0.7) | V1 |
| Sonic Rush Adventure | yes | 100 | 1.0 / 1.1 | 2 / 0 | 19 (17) | 1 | 1 / 0 | 0 | 0 () | 3 (0) | 24 | 155 / 0 / 0 | 0.9 | 0.0 (2.7) | V1 |
| Spore Creatures | no | 100 | 0.0 / 0.0 | 0 / 0 | 0 (0) | 1 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.1 | 0.0 (0.0) | V1 |
| Super Mario 64 DS | no | 100 | 0.0 / 0.0 | 4 / 0 | 11 (7) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 3 / 0 / 0 | 0.0 | 0.0 (0.0) | V1 |
| Super Princess Peach | yes | 100 | 0.8 / 3.2 | 200 / 66 | 341 (141) | 0 | 1 / 0 | 0 | 0 () | 12 (0) | 47 | 0 / 0 / 0 | 12.0 | 0.0 (22.3) | V1 |
| Tetris DS | yes | 100 | 0.0 / 3.7 | 44 / 0 | 63 (19) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.0 | 0.7 (0.7) | V1 |
| Trauma Center - Under the Knife 2 | yes | 99 | 0.0 / 35.1 | 28 / 0 | 493686 (493658) | 2 | 1 / 0 | 100 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 2.6 | 0.0 (4.7) | V2 |
| WarioWare - Touched! | no | 100 | 5.0 / 4.0 | 0 / 0 | 0 (0) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.1 | 0.3 (0.3) | V1 |
| World Ends with You, The | no | 100 | 9.0 / 9.0 | 0 / 0 | 0 (0) | 0 | 1 / 0 | 0 | 0 () | 0 (0) | 0 | 0 / 0 / 0 | 0.4 | 0.0 (0.3) | V1 |
| Yoshi's Island DS | yes | 100 | 0.9 / 4.6 | 87 / 0 | 401 (314) | 0 | 1 / 0 | 0 | 0 (262) | 0 (0) | 8 | 0 / 0 / 0 | 0.0 | 13.3 (13.3) | V1 |


Where each run ended up (from the 5/10/15-minute screenshots):

| Title | RA set | Reached play | What the screenshots show |
|---|---|---|---|
| Animal Crossing - Wild World (USA) (Rev 1) | yes | yes | walking in town |
| Clubhouse Games (USA) (En,Fr,De,Es,It) (Rev 1) | yes | yes | (touch script) Spit card game |
| Cooking Mama (USA) | yes | partial | recipe menu; no cooking minigame seen |
| Cory in the House (USA) | yes | yes | walking in the White House |
| Diddy Kong Racing DS (USA) | no | yes | a race |
| Digimon World DS (USA) | yes | yes | overworld with dialog |
| Drawn to Life (USA) (En,Fr) | yes | partial | hero drawing editor (in-game creation, not platforming) |
| Ghost Trick - Phantom Detective (USA) (En,Fr,De,Es,It) | yes | yes | first scene, ghost-trick tutorial |
| Golden Sun - Dark Dawn (USA, Australia) (En,Es) | yes | yes | in town, equip menu open |
| Kirby - Canvas Curse (USA) | no | no | file/game select menu |
| Kirby - Squeak Squad (USA) | yes | yes | level 1 platforming |
| Kirby Super Star Ultra (USA) | yes | yes | Spring Breeze boss fight |
| Legend of Zelda, The - Phantom Hourglass (USA) (En,Fr,Es) | yes | no | (touch script) Nintendo WFC setup |
| Legend of Zelda, The - Spirit Tracks (USA, Australia) (En,Fr,Es) | yes | no | (touch script) name entry |
| Legendary Starfy, The (USA) | yes | yes | underwater stage |
| LEGO Star Wars - The Complete Saga (USA) | no | partial | cantina hub, character select |
| Mario & Luigi - Bowser's Inside Story (USA) (En,Fr,Es) | no | yes | opening scenes and overworld with dialog |
| Mario Kart DS (USA, Australia) (En,Fr,De,Es,It) | yes | yes | mission mode, then a race |
| Mario Party DS (USA) (Rev 2) | no | no | main menu / gallery |
| Mario vs. Donkey Kong - Mini-Land Mayhem! (Europe) (En,Fr,De,Es,It) (NDSi Enhanced) | yes | no | (touch script) Construction Zone user-name prompt |
| Metroid Prime Pinball (USA) | no | yes | pinball table |
| New Super Mario Bros. (USA, Australia) | yes | yes | World 1-1 |
| Nine Hours, Nine Persons, Nine Doors (USA) | yes | yes | opening scene (visual novel: reading is the gameplay) |
| Nintendogs - Best Friends (USA) | yes | no | (touch script) kennel welcome dialog |
| Pac'n Roll (USA) | yes | no | (touch script) first tutorial |
| Peggle Dual Shot (USA) | yes | yes | (touch script) in a level (pause dialog open) |
| Phoenix Wright - Ace Attorney (USA) | yes | yes | first trial (court scenes at 5 and 10 min) |
| Plants vs. Zombies (USA) | yes | no | (touch script) menu only |
| Pokemon - Black Version (USA, Europe) (NDSi Enhanced) | yes | yes | in the player's house (DS mode) |
| Pokemon - Platinum Version (USA) (Rev 1) | yes | no | (touch script) intro yes/no prompt |
| Pokemon - SoulSilver Version (USA, Australia) | yes | no | (touch script) intro |
| Professor Layton and the Curious Village (USA, Australia) | yes | yes | (touch script) first puzzle |
| Resident Evil - Deadly Silence (USA) | yes | yes | mansion 1F, exploring |
| Rhythm Heaven (USA) | yes | no | title screen ("Touch me!") at 5, 10 and 15 min |
| Sonic Colors (USA) (En,Ja,Fr,De,Es,It) | yes | yes | first act |
| Sonic Rush (USA) (En,Ja,Fr,De,Es,It) | yes | yes | first act |
| Sonic Rush Adventure (USA) (En,Ja,Fr,De,Es,It) | yes | yes | first act |
| Spore Creatures (USA) (En,Fr,De,Es,It,Nl) | yes | no | language menu |
| Super Mario 64 DS (USA, Australia) (Rev 1) | no | no | title screen (touch the picture) |
| Super Princess Peach (USA) | no | yes | first stage |
| Tetris DS (USA, Australia) | yes | yes | a Tetris game |
| Trauma Center - Under the Knife 2 (USA) | yes | yes | first operation |
| WarioWare - Touched! (USA) | no | no | start-up tutorial screen |
| World Ends with You, The (USA) | yes | no | title screen |
| Yoshi's Island DS (USA) (Rev 1) | no | yes | first stage |


## 4. P2 allow list

**The rules as set** (from the M1 brief):
1. no FIFO capture or DMA mode 4;
2. no DISPCNT A write covering byte 2 in the window, while live, other than a pure RMW;
3. DISPCNT A reads in the window are reported, not gated (section 4.3);
4. no absolute IE write after boot;
5. no raster register write (R16 hit) at lines 30-38.

Any hit is "no", even when the title didn't reach play (the hazard is in the game). A title with no hit that didn't reach play is "unknown".

**Lenient reading** (offered for the owner's ruling, not applied by default):
- An absolute write that keeps mode 1 only drops the strip for that frame. `hardware.md` 2.1 calls this harmless; it is one frame of missing notice, not garbage. So only mode-changing writes count.
- R16 hits count only above 1 write per 1,000 frames.

| Title | P2 allow list (rules as set) | Reason | Lenient reading | Reason |
|---|---|---|---|---|
| Animal Crossing - Wild World | no | raster writes at lines 30-38 | no | raster writes at lines 30-38 x224879 |
| Clubhouse Games | yes |  | yes |  |
| Cooking Mama | unknown | gameplay only partly reached | unknown | gameplay only partly reached |
| Cory in the House | no | raster writes at lines 30-38 | no | raster writes at lines 30-38 x84 |
| Diddy Kong Racing DS | no | raster writes at lines 30-38 | no | raster writes at lines 30-38 x22784 |
| Digimon World DS | yes |  | yes |  |
| Drawn to Life | no | raster writes at lines 30-38 | unknown | gameplay only partly reached |
| Ghost Trick - Phantom Detective | yes |  | yes |  |
| Golden Sun - Dark Dawn | no | DISPCNT A non-RMW write in the window while live x1 | no | mode-changing DISPCNT A write in the window x1 |
| Kirby - Canvas Curse | unknown | no gameplay reached | unknown | no gameplay reached |
| Kirby - Squeak Squad | yes |  | yes |  |
| Kirby Super Star Ultra | no | DISPCNT A non-RMW write in the window while live x2 | yes |  |
| Legend of Zelda, The - Phantom Hou | no | absolute IE writes x4 | no | absolute IE writes x4 |
| Legend of Zelda, The - Spirit Trac | unknown | no gameplay reached | unknown | no gameplay reached |
| Legendary Starfy, The | no | raster writes at lines 30-38 | no | raster writes at lines 30-38 x414468 |
| LEGO Star Wars - The Complete Saga | unknown | gameplay only partly reached | unknown | gameplay only partly reached |
| Mario & Luigi - Bowser's Inside St | yes |  | yes |  |
| Mario Kart DS | yes |  | yes |  |
| Mario Party DS | no | DISPCNT A non-RMW write in the window while live x114 | unknown | no gameplay reached |
| Mario vs. Donkey Kong - Mini-Land  | unknown | no gameplay reached | unknown | no gameplay reached |
| Metroid Prime Pinball | yes |  | yes |  |
| New Super Mario Bros. | yes |  | yes |  |
| Nine Hours, Nine Persons, Nine Doo | yes |  | yes |  |
| Nintendogs - Best Friends | unknown | no gameplay reached | unknown | no gameplay reached |
| Pac'n Roll | unknown | no gameplay reached | unknown | no gameplay reached |
| Peggle Dual Shot | yes |  | yes |  |
| Phoenix Wright - Ace Attorney | yes |  | yes |  |
| Plants vs. Zombies | unknown | no gameplay reached | unknown | no gameplay reached |
| Pokemon - Black Version | no | raster writes at lines 30-38 | no | raster writes at lines 30-38 x2880 |
| Pokemon - Platinum Version | unknown | no gameplay reached | unknown | no gameplay reached |
| Pokemon - SoulSilver Version | unknown | no gameplay reached | unknown | no gameplay reached |
| Professor Layton and the Curious V | yes |  | yes |  |
| Resident Evil - Deadly Silence | yes |  | yes |  |
| Rhythm Heaven | unknown | no gameplay reached | unknown | no gameplay reached |
| Sonic Colors | no | DISPCNT A non-RMW write in the window while live x1 | no | mode-changing DISPCNT A write in the window x1 |
| Sonic Rush | yes |  | yes |  |
| Sonic Rush Adventure | yes |  | yes |  |
| Spore Creatures | unknown | no gameplay reached | unknown | no gameplay reached |
| Super Mario 64 DS | unknown | no gameplay reached | unknown | no gameplay reached |
| Super Princess Peach | no | DISPCNT A non-RMW write in the window while live x66 | yes |  |
| Tetris DS | yes |  | yes |  |
| Trauma Center - Under the Knife 2 | yes |  | yes |  |
| WarioWare - Touched! | unknown | no gameplay reached | unknown | no gameplay reached |
| World Ends with You, The | unknown | no gameplay reached | unknown | no gameplay reached |
| Yoshi's Island DS | yes |  | yes |  |


### 4.1 Totals against M1

| Denominator | Rules as set: yes / no / unknown | Compatible | Lenient: yes / no / unknown | Compatible |
|---|---|---|---|---|
| 27 titles that reached play | 18 / 9 / 0 | **67%** | 20 / 7 / 0 | **74%** |
| 30 with a known verdict (rules) / 28 (lenient) | 18 / 12 | 60% | 20 / 8 | 71% |
| all 45 (unknown counts as not compatible) | 18 / 12 / 15 | 40% | 20 / 8 / 17 | 44% |

**M1 reading:** the result is between 50% and 80% on the denominators that exclude unknowns, so it is the owner's call. It doesn't reach the 80% "worth building" mark under either reading. It is also well clear of the 50% stop line, except when the 15 unknowns are counted as failures. Of the 18 titles that didn't fully reach play, 3 (rules) or 1 (lenient) showed a hazard anyway (Phantom Hourglass, Mario Party DS, Drawn to Life). The rest are unknown.

### 4.2 Why titles fail

**Raster at lines 30-38 (R16):** 6 titles.

| Title | What runs there | Fixable by another N? |
|---|---|---|
| Animal Crossing WW | HBlank IRQ every line, every frame. IRQ-mode writes to BG2/BG3 affine and scroll (`0x04000030-3C`), WIN0/1 (`0x40-42`), BLDCNT (`0x50`). ~225k writes in the band | No: every line |
| Legendary Starfy | HBlank IRQ on 64% of frames, every line; engine B BG0 scroll (`0x04001010/12`). Our exit IRQ delays engine-B raster too | No |
| Pokemon Black | HBlank IRQ every frame; IRQ-mode writes to WININ/WINOUT (`0x48/4A`, A and B), ~2.9k in the band | No |
| Diddy Kong Racing DS | IRQ-mode window writes (`0x40-44`), ~23k in the band (race HUD split) | Maybe |
| Cory in the House | SYS-mode BLDCNT/BLDALPHA writes, line-locked, 84 in 15 min | Probably heuristic noise; lenient still fails it (84 > 54) |
| Drawn to Life | One IRQ-mode WIN0H key, 9 writes | Noise-level; lenient passes it, but it only reached "partial" |

Five more titles keep the HBlank IRQ on for every line but write no display register in the band: Kirby Super Star Ultra, Mario Party DS, Nintendogs, Kirby Canvas Curse and Trauma Center 2. Their HBlank work, whatever it is, is delayed by our exit IRQ on line N-1 with no visible effect seen here. That should be checked in M7.

**DISPCNT A in the window while live (R1):** 5 titles.

| Title | Writes | Mode change? |
|---|---|---|
| Mario Party DS | 114 absolute 32-bit writes from PC `0201ed20` at lines 17-19. They rewrite layers and keep mode 1 | No (strip drops for that frame) |
| Super Princess Peach | 66 absolute writes from `02074c38` and `021d683c`, lines 6-30 | No |
| Kirby Super Star Ultra | 2 absolute 16-bit writes of byte 2-3 (same value) from `020157f8` | No |
| Golden Sun DD | 1 RMW-mode write at line 9: mode 1 → **2** (VRAM display), PC `0204f3c8` | Yes |
| Sonic Colors | 1 RMW-mode write at line 21, **from IRQ mode**: mode 1 → **2**, PC `020cf26c` | Yes |

The two mode-2 switches are mid-frame changes to VRAM display, probably transition effects. With the strip live, such an RMW reads mode 3. Then one of two things happens:
- if the code ORs its mode in, it writes 3 and the game's effect is lost on those lines;
- if it replaces bits 16-17, it writes 2 and the strip ends at that line.

Either way the visible result is wrong for that frame. Golden Sun's write lands at line 9, inside the strip itself.

**Absolute IE writes after boot (R15):** only Zelda Phantom Hourglass. 4 writes in 15 min from PC `0200c3dc`, during the Nintendo WFC setup screens the script wandered into (10 in the first pass's generic run). Every title also does one absolute IE write at boot (frames 4-30, crt0/`OS_InitIrqTable`). That is before the module could arm anything, but nds-bootstrap's IE hook must re-apply bits 28-31 after it.

**FIFO capture / DMA mode 4: none in 45 titles.** Display capture is common: 18 titles enable it, and Clubhouse, Peggle, Pac'n Roll and LEGO SW do so every frame. Golden Sun and Pokemon Black blend A+B. In every case source B is VRAM, never the main-memory FIFO. No CPU or DMA write to `0x04000068` was seen.

### 4.3 DISPCNT reads in the window (R2)

Reading byte 2 in the window is normal: 34 of 45 titles do it while live, and 30 have bare reads. Trauma Center 2 has ~490k (about 9 per frame) from one Thumb site. A disassembly pass over the saved code (`bare_reads.py`, 10 instructions after the load) classes almost all as SDK getters that mask other bit fields. Examples: `and #0x7000000` (char base), `and #0x38000000` (screen base), `and #7` (BG mode).

Sites whose following code may keep bits 16-17:

| Title | Site (count in window) | What the code does | Risk |
|---|---|---|---|
| Pokemon Black | `0204dc78` (99) | conditional loads, then AND with a literal-pool mask | mask not resolved; needs a look |
| Trauma Center 2 | `0200b3f5` Thumb (3,557), `0200d253` Thumb (1) | AND with a literal mask, then stored to the stack / the whole value stored to the stack | possible save of mode 3; the hot site `02007555` (487k) is also literal-masked and unresolved |
| Phantom Hourglass | 9 reads | mask keeps bits 16-17 | low count |
| MvDK Mini-Land Mayhem | 7 reads | mask keeps bits 16-17 | low count |
| M&L Bowser's Inside Story | 1 read | value stored by STRH (low half only) | probably harmless |

A save of mode 3 matters only if the game later writes it back. The survey can't see that cheaply. The `GXi_DispMode` check (`hardware.md` 2.1 (b)) and the guard IRQ at N+M-1 are the backstops. Recommendation: don't gate on reads. Gate on writes, and on the `GXi_DispMode` = 3 detector at run time.

## 5. IRQ handler variants (M2, R4)

The handler at DTCM+`0x3FFC` was in ITCM (`0x01FF8000-0x01FFFxxx`) in all 45 titles. DTCM was at `0x027E0000` (32), `0x027C0000` (10), `0x02FE0000` (2, the DSi-enhanced titles) or `0x023C0000` (1). Hashing the handler body up to its literal pool:

| Variant | Titles | Body | Code |
|---|---|---|---|
| V1 | 36 (80%) | 0x50 bytes + 2 literal words | NitroSDK release `OS_IrqHandler`: `stmdb sp!,{lr}`; `mov ip,#0x04000000`; `add ip,ip,#0x210`; IME test with `ldmeq sp!,{pc}`; `ldm ip,{r1,r2}`, `ands`; clz loop; `str r1,[ip,#4]` (IF ack); `ldr r0,[table, r0 lsl 2]`; `ldr lr,=OS_IrqHandler_ThreadSwitch`; `bx r0`. The literals (table address, return address) vary per game |
| V2 | 7 | 0x60 bytes | Same logic, compiled without conditional returns: `beq`/`b`/`ldm sp!,{pc}` pairs. M&L BIS, 999, Peggle, Pokemon Platinum, Layton CV, Rhythm Heaven, Trauma Center 2 (2007-2009 titles) |
| V3 | 1 | | Diddy Kong Racing DS: no `stmdb` at entry (`bxeq lr` returns), plus a special case `cmp r0,#0xa` (IRQ 10) that skips the `lr` set-up |
| V4 | 1 | | Golden Sun DD: a compact TwlSDK-style handler (`ldr r2,[ip,#0x208]!`, `ldrd r0,r1`, conditional chain, `blx lr`) that falls through into the thread-switch code |

- Raw 0x80-byte hashes: 37 variants. They split on the function that follows the handler.
- With literal words masked: 7.

**M2 reading:** two signatures (V1 and V2) cover 43 of 45 titles (96%), so M2 passes for `OS_IrqHandler`. Adding V3 and V4 covers all 45, but each is a one-off and might be better served by the BIOS-vector wrapper alternative in R4. The dispatcher's IF-ack/table pattern (from `clz` to `str r1,[ip,#4]`) is byte-identical in V1, V2 and V3. A signature anchored there would cover 44 of 45, but the patch point differs between them. Dumps (`SLUG.irq.bin`) and `variants.tsv` are in the session scratchpad (`survey/irqdumps3/`). They contain ROM bytes, so they aren't committed. `GX_DispOff` wasn't censused (D3 is the static scan).

## 6. Black stretches (M3, P3)

Strict definition (forced blank, mode 0 or no layers at line 0, >= 30 frames, boot excluded):
- Only 10 titles had any stretch, and 5 had >= 1 per 5 min: Yoshi's Island DS 13.3, Mario Party DS 4.7, NSMB 4.7, Kirby SSU 3.0, M&L BIS 1.3.
- The median title has **0**. **M3 fails** as defined.

Most DS games fade through master brightness rather than forced blank. Counting full fades (factor 16) as black frames:
- 20 of 45 titles have >= 1 per 5 min;
- the median is 0.67 over all 45 and **2.0 over the 27 that reached play**;
- Starfy reaches 51, MvDK 26, Pokemon Black 27, Super Princess Peach 22 and RE:DS 17.

So P3 is viable only if the card is drawn during full-brightness fades as well as forced blanks (and, if master brightness also dims the card, the card would need engine-B or other means). That is a design question for the owner. Fade (factor >= 12) frames are common in some titles: Starfy 17%, GS 13%, Pokemon Black 12%, SPP 12%, MvDK 11.5%, M&L 10%. The P2 guard "MASTER_BRIGHT A factor < 12" would defer notices during those.

## 7. Caveats

- **Gameplay coverage.** 18 of 45 titles never reached core play, or only partly. They count as "unknown", not "compatible". The generic script gets stuck on name entry (both Zeldas, Peggle in the generic run, MvDK, SM64DS's touch title), touch prompts (Nintendogs, Pac'n Roll, Pokemon Platinum/SoulSilver intros, Rhythm Heaven, WarioWare), language menus (Spore) and main menus (PvZ, Kirby CC, Mario Party, TWEWY).
- **Script-reached play is the first minutes of a game.** Titles judged compatible (for example NSMB, Mario Kart, Sonic Rush) may do mid-frame DISPCNT or raster work in levels or modes not reached. This is a lower bound on incompatibility.
- **Timing is the emulator's.** A game's main-loop write that lands at line 25 here may land in a different place on hardware, so window and band counts near the edges (lines 33-38, 259-262) are uncertain by a few lines.
- **The RMW test is temporal, not semantic:** a byte-2 read within 2 lines before a write. A game that reads DISPCNT into a shadow early and writes the shadow back later shows as absolute, which is correct. A game that reads, waits more than 2 lines, then writes shows as absolute too, which is conservative.
- **"Live" assumes P2 arms only after 30 stable mode-1 frames.** If the owner shortens that, more of the boot and transition writes would count.
- **Only the ARM9** is measured. ARM7 DISPSTAT/VCOUNT use doesn't affect the ARM9 strip, but isn't reported.
- **ROM set.** USA dumps from one library plus one European (MvDK). 35 of the 45 have RA sets. There are no DSi-exclusive titles, by design (DS mode).
- **Not measured here** (dropped from scope by the coordinator): VBlank wait method and last-halt line, which the idle-coverage census covers. Also not measured: VRAMCNT, POWCNT1 changes and engine-B layers (M4), and `GX_DispOff` calls by signature.


## Re-score for the bottom-edge strip (display-library.md section 11)

The tables above score the **top-edge** window (lines 261-35). The owner's zero-lag rule then moved the strip to the **bottom edge**: lines 158-189 (N = 32, K = 2). It is shown only while the game waits for VBlank, and ends the moment any game IRQ arrives. That changes what counts:

| Factor | Effect on the bottom strip |
|---|---|
| Game code touching DISPCNT in the band | Doesn't count: the strip isn't shown while the game runs |
| Game IRQs in the band (H-Blank/VCOUNT raster) | Abort the strip. A title with raster IRQs there on more than 10% of frames gets no strip |
| Legacy DMA writes to DISPCNT A in the band (H-Blank DMA tables) | Raise no IRQ, so they would fight the mode switch: fail. None of the 45 titles does this |
| DMA writes to other display registers in the band | Harmless: the strip replaces whole lines |
| FIFO capture / DMA mode 4 | Fail. None of the 45 titles uses them |

Produced by  from the same logs; no new emulator runs.

**Accuracy:** "raster IRQs in band on N% of frames" uses the run's count of frames with any H-Blank/VCOUNT IRQ, gated on there being any in the band. It is an upper bound for titles whose raster IRQs sit mostly outside the band.

| Title | Reached play | Bottom strip | Notes |
|---|---|---|---|
| Animal Crossing   Wild World  USA   Rev 1 | yes | no: raster IRQs in band on 100% of frames | DMA writes other display registers in band (76), harmless |
| Clubhouse Games  USA   En Fr De Es It   Rev 1 | yes | no: raster IRQs in band on 100% of frames |  |
| Cooking Mama  USA | partial | unknown (partial play) |  |
| Cory in the House  USA | yes | yes |  |
| Diddy Kong Racing DS  USA | yes | yes | occasional raster IRQ in band: strip skips those frames |
| Digimon World DS  USA | yes | yes |  |
| Drawn to Life  USA   En Fr | partial | unknown (partial play) | occasional raster IRQ in band: strip skips those frames |
| Ghost Trick   Phantom Detective  USA   En Fr De Es It | yes | yes |  |
| Golden Sun   Dark Dawn  USA  Australia   En Es | yes | yes |  |
| Kirby Super Star Ultra  USA | yes | no: raster IRQs in band on 100% of frames |  |
| Kirby   Canvas Curse  USA | no | unknown (didn't reach play) |  |
| Kirby   Squeak Squad  USA | yes | yes |  |
| LEGO Star Wars   The Complete Saga  USA | partial | unknown (partial play) |  |
| Legend of Zelda  The   Phantom Hourglass  USA   En Fr Es | no | unknown (didn't reach play) | absolute IE writes after boot: 4 (R15) |
| Legend of Zelda  The   Spirit Tracks  USA  Australia   En Fr | no | unknown (didn't reach play) |  |
| Legendary Starfy  The  USA | yes | no: raster IRQs in band on 65% of frames | DMA writes other display registers in band (674496), harmless |
| Mario Kart DS  USA  Australia   En Fr De Es It | yes | yes |  |
| Mario Party DS  USA   Rev 2 | no | unknown (didn't reach play) |  |
| Mario   Luigi   Bowser s Inside Story  USA   En Fr Es | yes | no: raster IRQs in band on 64% of frames | DMA writes other display registers in band (3456), harmless |
| Mario vs  Donkey Kong   Mini Land Mayhem   Europe   En Fr De | no | unknown (didn't reach play) |  |
| Metroid Prime Pinball  USA | yes | yes |  |
| New Super Mario Bros   USA  Australia | yes | yes | DMA writes other display registers in band (1651356), harmless |
| Nine Hours  Nine Persons  Nine Doors  USA | yes | yes |  |
| Nintendogs   Best Friends  USA | no | unknown (didn't reach play) |  |
| Pac n Roll  USA | no | unknown (didn't reach play) |  |
| Peggle Dual Shot  USA | yes | yes |  |
| Phoenix Wright   Ace Attorney  USA | yes | yes |  |
| Plants vs  Zombies  USA | no | unknown (didn't reach play) |  |
| Pokemon   Black Version  USA  Europe   NDSi Enhanced | yes | no: raster IRQs in band on 100% of frames |  |
| Pokemon   Platinum Version  USA   Rev 1 | no | unknown (didn't reach play) |  |
| Pokemon   SoulSilver Version  USA  Australia | no | unknown (didn't reach play) |  |
| Professor Layton and the Curious Village  USA  Australia | yes | yes |  |
| Resident Evil   Deadly Silence  USA | yes | yes | DMA writes other display registers in band (51), harmless |
| Rhythm Heaven  USA | no | unknown (didn't reach play) |  |
| Sonic Colors  USA   En Ja Fr De Es It | yes | yes | DMA writes other display registers in band (3000564), harmless |
| Sonic Rush Adventure  USA   En Ja Fr De Es It | yes | yes | DMA writes other display registers in band (3788028), harmless |
| Sonic Rush  USA   En Ja Fr De Es It | yes | yes | DMA writes other display registers in band (25344), harmless |
| Spore Creatures  USA   En Fr De Es It Nl | no | unknown (didn't reach play) |  |
| Super Mario 64 DS  USA  Australia   Rev 1 | no | unknown (didn't reach play) |  |
| Super Princess Peach  USA | yes | yes | DMA writes other display registers in band (32446872), harmless |
| Tetris DS  USA  Australia | yes | yes |  |
| Trauma Center   Under the Knife 2  USA | yes | no: raster IRQs in band on 100% of frames |  |
| WarioWare   Touched   USA | no | unknown (didn't reach play) |  |
| World Ends with You  The  USA | no | unknown (didn't reach play) | occasional raster IRQ in band: strip skips those frames |
| Yoshi s Island DS  USA   Rev 1 | yes | yes | DMA writes other display registers in band (153936), harmless |

Bottom-edge strip: 20 of 27 titles that reached play are compatible (74%); 45 titles total, 18 unknown.


**Still needed before any title is on the allow list:**
- **The game must be waiting for VBlank by line 156 on most frames.** That comes from idle-coverage's census ( per frame).
- **Titles without the SDK idle-thread wait get nothing today:** Golden Sun DD, the Sonic games and RE:DS (perf session), whatever their row says here.
- **Kirby Super Star Ultra,** our main test game, has raster IRQs in the band on every frame, so the bottom strip would never show there. Its test value for the strip is nil.
