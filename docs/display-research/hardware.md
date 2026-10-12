# Putting pixels in front of the player on a DSi: a hardware enumeration

Research of 2026-10-12. Read-only. It builds on `docs/overlay-research.md` (OR), `docs/handoff-overlay-ideas-vram.json` (JSON), `docs/hardware-access.md` and `docs/architecture.md`, and does not repeat them. Owner's stance: aim high, graphical, separate from the game's rendering if need be, with bottom-screen text as an acceptable fallback.

Sources:
- GBATEK: local copy `(local scratch)`. Line numbers match the ones OR cites.
- nds-bootstrap: `external/nds-bootstrap/retail/...` (paths below are relative to `retail/`).
- melonDS fork: `tools/melonDS/src`.
- Bakkerrrs fork: a shallow clone of `github.com/Bakkerrrs/Retroachievements-DS-on-3DS`, `docs/retroachievements.md` (BK:line).

---

## 0. What's new versus the prior research (corrections and challenges)

1. **The FIFO strip's exposure window is about 4x larger than OR says.** OR section 4 has the module set DISPCNT mode 3 "at VBlank, in the existing IPC IRQ". OR measured that IRQ landing at VCOUNT 192-231. From there to line N-1 the game reads mode 3: about 32-70 VBlank lines plus N, not N. The VBlank period is exactly when NitroSDK games do their display-register work, including `GX_DispOff`/`GX_DispOn` (the `sDispMode` leak OR found). Fix: enter mode 3 as late as possible, at line 261-262 (section 2.1). That needs a second precisely timed event per frame.

2. **VCOUNT-IRQ timing (OR's chosen variant 4c) has hardware evidence against it beyond the colour-LUT blacklist.** On real hardware, Bakkerrrs found Contra 4 clears `DISPSTAT` bit 5 (the Y-trigger IRQ enable) "constantly". Their VCOUNT hook fired on about 8% of frames (BK:1906-1915, BK:5285-5297). For a strip this is worse than a missing notice: if the switch back to mode 1 is lost, the rest of the frame shows FIFO underrun. nds-bootstrap's own `colorLutVCountBlacklist` (`arm9/include/colorLutBlacklist.h:272-293`) adds 21 titles, with "crashes after splash screens", "crashes when opening a stage" and so on. A game-independent timing source is needed (section 2.1: NDMA as a private timer).

3. **VRAMCNT is very probably readable on hardware**, despite GBATEK's "(W)" (`gbatek.txt:7081-7090, 7475-7483`). Evidence:
   - nds-bootstrap's in-game menu reads `VRAM_C_CR`/`VRAM_H_CR`, changes them, and writes the read values back on exit (`cardenginei/arm9_igm/source/inGameMenu.c:893-894, 906-908`). The screenshot code picks a capture bank by reading VRAMCNT A-D (`inGameMenu.c:283-299`).
   - If reads returned 0, every game that maps C or H would lose those banks after opening the menu. That would be widely reported, and it isn't.
   - melonDS returns them (`NDS.cpp:2849-2858, 2970-2974, 3078-3080`).
   - OR and JSON relied on "write-only" to reject bank-based ideas. Readability shows a bank's *current* mapping, not the game's *intent*, so the conclusion mostly survives. But detection gets much better. Hardware test H4 settles it.

4. **DISPCNT takes 8-bit writes** (melonDS `GPU2D.cpp:256-275`, and I/O generally accepts STRB). The mode bits 16-17 sit in byte 2 (bits 16-23), together with the VRAM-display block (18-19) and the OBJ boundary/H-Blank bits (20-23). The NSMB extended-palette bits (30-31) and the Kirby BG-enable bits (8-12), which OR measured being clobbered by the 32-bit NDMA write (4a), are in bytes 3 and 1. A `STRB` to `0x04000002` is the smallest possible footprint for the mode switch. The CPU must still do the read-modify-write; NDMA can't write bytes.

5. **A second, independent LED exists and nds-bootstrap already drives it:** the camera LED, `BPTWL[31h]` (00h off, 01h on, 02h hardware blink) (`gbatek.txt:31594-31620`). nds-bootstrap's "ROM read LED" option writes it (`cardenginei/arm7/source/cardengine.c:712,727`). It doesn't share a register with the Atheros SDIO-enable bit, unlike `BPTWL[30h]` bit 4. It has a hardware blink, so one I2C write per state instead of one per edge. The 3DS MCU emulates it in DSi mode ("31h R/W Camera LED (bit0,1)", `gbatek.txt:~31640`). The Wi-Fi LED cue and the power-LED colour don't work there.

6. **Measured idle slack can gate every on-screen mechanism.** The module already runs in the game's idle thread (OS_Halt hook), so it knows how much idle time each frame had. Show a bus-costing notice only while the last K frames each had idle time above the notice's worst-case cost. Then a lag frame caused by the notice needs the game's load to jump by more than that margin within one frame. That is a concrete reading of "no added lag frames", not a hope.

7. **NDMA IRQs don't require a write into game DTCM.** OR rejected variant 4b because NitroSDK's `OS_IRQTable` has 22 entries, so bits 28-31 jump through garbage. The fix it considered was wrapping the DTCM vector. The cleaner fix is a boot-time code patch of `OS_IrqHandler`, found by signature, that routes IF bits 28-31 to the module. That is the same class of change as the existing `OS_Halt` patch, and the patch nds-bootstrap already makes to the game's IRQ-enable function (`bootloaderi/source/arm7/find_arm7.c:116-120`; `cardenginei/arm9/source/cardengine.c:1755-1799` `myIrqEnable`). SCFG_EXT9 bit 8 is ours to set before the lock: the DS-mode branch writes `0x8300C000|BIT(16)` then locks (`bootloaderi/source/arm9/main.arm9.c:429-436`). DSi-mode games already run with bit 8 set (`0x8307F100`, :416).

---

## 1. Ground rules for judging (restated precisely)

"Game state" in this report means:
- main RAM, DTCM and ITCM;
- VRAM A-I, palette RAM, OAM;
- every display/DMA/timer/IRQ register the game can read or write: DISPCNT A/B, DISPSTAT, BGxCNT, the window/blend registers, MASTER_BRIGHT, DISPCAPCNT, POWCNT1, VRAMCNT, legacy DMA 0-3, TIMER 0-3, IE/IF/IME.

Things an NTR game can't see on a DSi:
- NDMA 0-3 (`0x040041xx`, SCFG_EXT9 bit 16);
- IE bits 24-31 on the ARM9 (only with SCFG_EXT9 bit 8);
- the BPTWL I2C registers;
- NWRAM outside what nds-bootstrap gives the game;
- the DSP, camera and SNDEXCNT;
- the module's own RAM at `0x02D80000+`.

**No mechanism that shows pixels on a DSi avoids writing at least one game-visible display register.** Every pixel path goes through engine A's `Select Video Input` (DISPCNT A bits 16-17) or through engine B's own layers (`gbatek.txt:8185-8235`, block diagram). Master brightness is applied after that selector, so it also covers anything we show. So the honest floor for "graphical on DSi" is: one byte of DISPCNT differs from the game's value for some lines per frame. Everything below is ranked against that floor.

---

## 2. Mechanisms

### 2.1 Engine A Main Memory Display strip, rebuilt ("4f"). Best on-screen candidate

**Pixels:** DISPCNT A mode 3 shows 15-bit pixels from the Main Memory Display FIFO (`0x04000068`), 4 words (8 pixels) per request, and bypasses all BG/OBJ/3D/effects. Master brightness still applies (`gbatek.txt:7989-7995, 8187-8198`). The FIFO transfer "starts at next frame", so the strip sits at the top (lines 0..N-1). The NDMA feeding it uses ARM9 startup mode 09h.

**Evidence that 09h is the FIFO, stronger than OR stated.** GBATEK hedges ("Work RAM (what?) (=probably Main memory display)", `gbatek.txt:27968`). But the NDMA ARM9 startup list is the legacy ARM9 DMA list re-encoded, entry for entry:

| Legacy | NDMA | Meaning |
|---|---|---|
| 1 | 06h | VBlank |
| 2 | 07h | HBlank |
| 3 | 08h | display sync |
| 4 | **09h** | main-memory display |
| 5 | 04h | card |
| 7 | 0Ah | geometry FIFO |

melonDS encodes exactly this table (`DSi.cpp:52-60`). It is an inference, not proof. Test H1.

**Revised per-frame sequence.** This is new: the timing comes from NDMA alone, with no VCOUNT and no game timers.
1. **Boot (bootloaderi, our fork):**
   - set SCFG_EXT9 bit 8 before the lock;
   - patch `OS_IrqHandler` (by signature per SDK version) so IF bits 28-31 call the module;
   - extend the existing irq-enable patch so IE bits 28/30 are ORed back like `IRQ_IPC_SYNC` is today (`misc.c:128-133`);
   - move CE9 card reads to NDMA1 only (today it uses 0, and 0/1 alternately, `cardDma.thumb.c:230,442,462`), so the FIFO gets NDMA0, the highest priority.
2. **IPC IRQ at VBlank (existing hook, VCOUNT about 192-231):** if a notice is due and the guards pass (below), start NDMA2 as a **one-shot delay timer**: start immediately, fill-data source, destination a private word, WCNT=2, physical block 1 word. BCNT gives the interval, with prescaler 1 (8.38 MHz, max 65535 ticks, about 7.8 ms, about 123 lines) set to land at line 261. IRQ on completion. NDMAxBCNT "allows to insert a delay after each (physical?) block" (`gbatek.txt:27920-27925`). This is a timer the game can't see and doesn't own.
3. **NDMA2 IRQ at about line 261:**
   - re-check the guards;
   - arm NDMA0: mode 09h, source = banner, (N+M)*128 words, 4-word blocks, repeat until TCNT, no IRQ;
   - `STRB [0x04000002], (b & ~3) | 3`;
   - re-arm NDMA2 as an H-Blank counter: startup 07h, fill-data, private destination, TCNT = N-1, IRQ.
4. **NDMA2 IRQ at the end of line N-1:** if the byte-2 mode bits still read 3, `STRB` them back to 1 (only bits 16-17 change). NDMA0 runs out the M margin lines by itself; a late IRQ only thickens the banner's bottom border.
5. **Next VBlank:** if the mode reads 3 and we didn't arm it, put back 1.

**Guards** (all reads of R/W registers or our own state):
- DISPCNT A mode == 1;
- DISPCAPCNT not enabled with source B = FIFO (`gbatek.txt:8136-8160`);
- no legacy DMA channel in start mode 4;
- MASTER_BRIGHT A factor below about 12, since a fade would hide the strip anyway;
- the display has been stable for T frames;
- **idle slack in each of the last K frames is at least the strip's worst-case bus cost** (section 0 item 6);
- the game isn't on the per-title deny list.

**What the game sees, and the residuals:**
- **DISPCNT byte 2 reads mode 3 for about N+2 lines a frame** (line 261 to N-1), versus about 70+N in OR's design. A game RMW of DISPCNT in that window keeps our 3 and our IRQ puts it back: harmless. An absolute write of mode 1 drops the strip for that frame: harmless.
- **The dangerous reader is `GX_DispOff`,** which saves the mode into `GXi_DispMode`/`sDispMode` in game RAM (OR's pokediamond finding). Two mitigations:
  - (a) a boot-time patch of `GX_DispOff` (by signature) that saves 1 when it reads 3 while the module's "strip live" flag is set. Same class as the `OS_Halt` patch.
  - (b) without a patch, read `GXi_DispMode` (read-only, address taken from `GX_DispOff`'s literal pool) at each guard. If it ever reads 3, a leak happened. The line-261 guard then sees mode 3 that we didn't set and puts back 1 before line 0. A `GX_DispOn` during VBlank then costs nothing visible. One during lines 0-191 still shows one frame of underrun.
- **IRQs:** 3 extra IRQ entries per shown frame, through the game's dispatcher (about 300-900 ARM9 cycles each, measured by OR at 750-880 for the IPC hook). About 0.1-0.2% of a frame. The game's other IRQs are delayed by at most one handler's length, a few microseconds.
- **Bus.** From the GBATEK timing table (`gbatek.txt:7620-7640`):
  - **From main RAM:** a 4-word block costs N32 + 3xS32 = 10 + 6 = 16 bus cycles to read, plus about 4 to write the FIFO. That is about 20 of every 48 bus cycles (8 dots x 6) on strip lines, about 42%. For N+M = 28 lines that is about 25k of about 560k bus cycles per frame, **about 4.5% of the frame's bus time**, and only while a notice is up. It also contends with the ARM7 (sound) on main RAM.
  - **From NWRAM (32-bit, 1-cycle):** about 8/48, so **about 1.8%**, and no ARM7 contention.
  - The ARM9 only stalls when it needs the bus during a 4-word block (cache/TCM hits run on). So the cost to the game is at most that, and with the slack gate it shouldn't cost a frame.
  - Whether NDMA 09h accepts an NWRAM source is unknown (legacy mode 4 says "source must be Main Memory", `gbatek.txt:8193`). That is test H5.
- **The visual cost is the notice itself:** an opaque 256xN band over the top of the engine-A panel, translucency impossible. The panel follows POWCNT1.15, which the game owns.
- **Code patches:** `OS_IrqHandler` (required), `GX_DispOff` (optional), CE9 channel move. All boot-time, in the nds-bootstrap "hook point" class.

**Memory:** a 256xN banner is 12 KB at N=24, so 24 KB double-buffered. Plus a font (about 4-8 KB) and badges at 32x32 RGB555 (2 KB each; the launcher can pre-convert them into `session.bin` or a side file). About 40-60 KB in the module heap.

**Universality:** any game whose engine A is in mode 1 and which doesn't use FIFO capture or DMA mode 4. FMV players such as Mobiclip mostly use BG bitmaps, not the FIFO; see M1. It doesn't depend on whether the game uses VCOUNT, HBlank or timers. It needs an `OS_IrqHandler` signature hit, which nds-bootstrap's patcher can find the way it finds `irqEnable` today. It works the same on a 3DS in TWL mode.

**Verdict:** the only on-screen DSi mechanism whose game-visible footprint is one byte of one register, for a few lines, plus measurable bus time. It isn't strictly zero-cost. The smallest residual is: DISPCNT byte 2 is 3 instead of 1 for about N+2 lines a frame; about 0.1-0.2% ARM9 for IRQ entries; 1.8-4.5% of bus time while shown; a boot-time dispatcher patch. **Go for a hardware experiment, then opt-in per game with an allow list.** Rule the original 4c VCOUNT variant out as primary timing; keep it only as a fallback for games where the dispatcher patch isn't found and the M1 trace shows no VCOUNT/DISPSTAT ownership.

**If H1 fails (09h isn't the FIFO):** the only other FIFO feeder is legacy DMA mode 4, a game channel. Then fall to 2.2.

### 2.2 Engine A VRAM Display strip (mode 2) from a free bank A-D

**Pixels:** DISPCNT A mode 2 shows a 256x192 direct-colour bitmap from the bank selected by DISPCNT bits 18-19, which should be in LCDC mode (`gbatek.txt:7989-7995, 8162-8166`). Same mid-frame switch as 2.1, same timing chain, but no FIFO and no main-RAM bus cost: the display reads VRAM, which it does anyway (`gbatek.txt:7848-7852`, VRAM waitstates).

**What it touches:** DISPCNT byte 2 (bits 16-19), plus 512xN bytes inside a bank A-D. The bank must currently be disabled (VRAMCNT bit 7 = 0) or LCDC (MST = 0) and not used by the game. If the game uses capture with source B = VRAM, bits 18-19 are its capture-read block, so we must stand down whenever DISPCAPCNT is enabled.

**Detection:** VRAMCNT readback (if H4 passes) shows the current mapping. "Not used" can only come from traces (M4) or by reading NitroSDK's own VRAM bank bookkeeping in game RAM, read-only (`GX_SetBankFor*` keeps the LCDC/disabled set in a global).

**Wrong guess:** if the game later maps the bank, its VRAMCNT write wins and our strip shows the game's data for one frame (cosmetic). If the bank holds parked data the game expects to keep (the Ketsui failure, BK:8510-8540), we corrupt it. That is real game state.

**Verdict:** a fallback only if H1 fails. More universal is unlikely, since 3D games use A-D for textures and 2D games for BG/OBJ. It also writes game VRAM. Allow-list only.

### 2.3 "Black-frame card": full-screen notices when the game presents nothing

**Idea:** engine A frames that show only a uniform colour carry no information we could hide. That covers:
- forced blank (DISPCNT bit 7, the engine outputs white);
- all of bits 8-12 clear (backdrop only), with standard BG palette entry 0 read to know the colour;
- display mode 0 (white).

Scene changes and loads produce such frames. On those frames, show a large card (full-width, for example 256x96 with a 64x64 badge) with the 2.1 machinery. Queue unlock notices for the next black stretch, and use the strip for everything urgent.

**Proof of "unused this frame":** at the line-261 guard the registers say the engine will output a uniform colour for line 0 onward. **Wrong guess:** the game turns a layer on mid-frame, by a raster effect or a thread write during lines 0-191. We then hide its first visible lines for one frame. During fade-ins from black those lines are dark anyway.

**Cost:** FIFO bus for 96 lines (or 192 for a full screen) is about 15-30% of frame bus time *while the game is loading*, which can stretch a load slightly. The slack gate (idle time) applies, but loads are often card-bound (nds-bootstrap SD/NDMA), so measure (M3, H6).

`GX_DispOff`/`GX_DispOn` are called exactly around these frames, so the `GX_DispOff` patch (2.1, mitigation a) is mandatory here.

**Verdict:** a creative complement to 2.1 that gives big, badge-rich notices at almost no visual cost. Same hardware unknowns as 2.1. Worth prototyping in melonDS after M3 shows black stretches are common (pass mark below).

### 2.4 Free BG layer or OBJ slots (Bakkerrrs path), with runtime detection

**What Bakkerrrs learned on hardware** (BK:5530-5810, 8430-8720):
- BG path: taking the first disabled sub layer is invisible if a lower-numbered layer has priority 0. Displacing an enabled priority-0 layer worked, but hid scenery for 3 s (Contra 4).
- OBJ path: OAM entries taken from the front stole the game's next sprites (bullets, the player vanished). Entries are only safe from index 127 downward, behind the game's own sprites.
- "Disabled entry means free tiles" was false: parked banks were reused at stage transitions, giving garbage plus game graphics breakage on Ketsui.
- Games DMA tiles and palettes every frame, so the overlay had to re-assert pixels and palette every frame, guarded by probe words.
- Their own summary (BK:8466-8478): "which object VRAM and which OAM entry is this game not using ... has no reliable answer from outside the game. Every fix so far has been a better inference."
- The Ketsui freeze turned out to be an SD-write collision, not the overlay (BK:8352-8360). The garbage was real.

**What better detection could add:**
- VRAMCNT readback (if H4 passes) to know which bank backs `0x06600000`.
- The NNS G2D OAM managers' ranges in game RAM, read-only: entries outside every manager's range are never written by `NNS_G2dApplyOamManagerToHW`, which applies only its own range.
- Per-title write traces (M1) of OAM/OBJ-VRAM/palette ranges.

None of these proves future non-use. A game can clear the whole OAM, or load characters at fixed offsets outside any manager.

**Verdict:** violates "never touch game OAM/VRAM/palettes" by construction. The residual on a wrong guess is visible corruption of the game's sprites or tiles, proven on hardware. Allowed only as a per-title opt-in, built from full-playthrough traces, and never as the default. Below 2.1 in every respect except that it can target engine B (the bottom screen).

### 2.5 Bottom-screen (engine B) approaches, the owner's accepted fallback

Engine B has no FIFO, no VRAM display, no capture and no 3D (`gbatek.txt:7980-8000, 8185-8235`). Every engine-B pixel needs an engine-B layer or OBJ, VRAM C/D/H/I, and engine-B palette/OAM. So an engine-B notice always writes game-owned display state. The least-touching design:

**"Blank sub screen" mode:**
- *Trigger:* DISPCNT_B has bits 8-12 all clear (backdrop only, R/W readable) for T frames, **and** bank H or I reads back disabled (bit 7 = 0, which needs H4).
- *Draw:* map that bank as sub BG (H at `0x06200000`, I at `0x06208000`). Skip if C is mapped to sub BG, because they overlap. Point one sub BG layer's BGCNT at it, write 16 palette entries, and enable that layer bit.
- *Save/compare/restore:* for every word we overwrite (BGCNT, the DISPCNT_B layer bit, the palette entries, VRAMCNT_x), keep (original, ours).
  - Each VBlank, compare live against "ours". If something differs, the game wrote it: drop it from our set, never restore it, and end the notice within a frame.
  - On exit, restore only words still equal to "ours".
  - The disabled bank's contents are undefined to the game, so writing them is the one write the game can't depend on. Parked data in a *disabled* bank is theoretically possible; M4 checks it.

**What the game can see:**
- its own registers holding our values for up to 3 s;
- palette RAM entries changed, if it ever reads palette RAM, which is rare;
- if it starts using the sub screen during the notice, our text on its first frame over its content (1 frame).

**Universality:** limited to games that blank the sub screen during play. Many show a map or status there; M1 measures it.

**Takeover of a static (non-blank) sub screen:** worse. The game's VRAM is overwritten, so any game write during the notice is lost or clobbered. That is the in-game-menu model without the pause. Rejected.

**Verdict:** an acceptable *fallback* under the owner's stance only with the compare/restore discipline and an allow list. It touches game registers and palette. The residual is a 1-frame overdraw if the game wakes the sub screen during a notice, plus register values the game could read back. Rank below 2.1 and the LEDs.

### 2.6 Display capture compositing

DISPCAPCNT (R/W, `gbatek.txt:8136-8160`) can blend source A (the game's graphics) with source B (the FIFO) at EVA/EVB into a bank A-D in LCDC mode, and mode 2 shows it. Capture writes line L while it is produced, so showing the composite always lags a frame. That is +16.7 ms of input latency on engine A for as long as a notice is up. It also needs a free 128 KB bank (or 96 KB at 256x192), and it fights games that capture.

**Which games capture:** NSMB per OR; dual-screen-3D titles; motion-blur effects. DISPCAPCNT bit 31 is readable, so M1 can count them.

**Niche variant:** in games that already show a captured frame (dual-screen 3D shows last frame's capture on the other screen), the extra frame of latency already exists by design. After capture ends at line 192, we could write the banner into the game's capture bank before it is shown next frame. Capture overwrites it every frame, so the game's *data* isn't lasting. But it is a write into game VRAM, and a game that blends with source B = VRAM would feed our pixels back.

**Verdict:** reject for general use (latency, bank). The niche variant is interesting but small and still a VRAM write.

### 2.7 Brief whole-screen or one-screen takeover (save/restore, as the in-game menu does)

nds-bootstrap's menu saves:
- DISPCNT_B, BG0-3CNT_B, VRAMCNT C/H, POWCNT and MASTER_BRIGHT_B;
- the sub map, palette and font tiles.

It forces POWCNT swap, writes the write-only BLD/MOSAIC registers as 0 because "can't back up", and later restores everything (`inGameMenu.c:887-930, 1110-1125`). The copies themselves cost well under a frame (a few KB at VRAM speed). The real cost is that the game has to be **paused**: while it runs it writes the same VRAM and registers, and write-only registers (BLDY, BLDALPHA, MOSAIC, WINxH/V) can't be restored at all.

**Verdict:** a non-paused version corrupts. A paused version breaks "no pausing" and costs as many frames as the notice lasts (for example 120 frames for 2 s), unless the player asked (the menu Now page, already planned). Reject for automatic notices.

### 2.8 Master brightness, windows, blending

- **MASTER_BRIGHT** is per engine and whole-screen, with no spatial control (`gbatek.txt:7836-7843`), and it is the game's fade control.
- **Windows:** WIN0H/V and WIN1H/V are write-only, so we can't restore them (`gbatek.txt:615, 625, 1258` as cited by JSON).
- **Blending:** BLDY and MOSAIC are write-only (the in-game menu's own comment, `inGameMenu.c:913-916`).
- None of them draws text.

The colour-LUT feature rewrites MASTER_BRIGHT, BLDCNT and palettes every frame (`arm9_colorlut/source/cardengine.c:16-80`). That shows upstream tolerates it, but by our rules it is game state.

**Verdict:** reject. One useful *read*: MASTER_BRIGHT A gates 2.1, because the strip fades with the game.

### 2.9 Per-frame H-Blank/VCount effects

- **Whole-frame H-Blank IRQ:** 4-7% CPU (OR 4d). Reject.
- **H-Blank IRQ for only N lines** (enable at line 261, disable in the handler at line N-1): about N IRQs per frame, about 0.4% at N=16. But DISPSTAT bit 4 and IE bit 1 are game-owned, and games that write DISPSTAT absolutely (Contra 4 clears bit 5 constantly, BK:5294) would kill it mid-strip. That loss shows as underrun. Second-tier timing source only.
- **ARM9 VCOUNT IRQ:** the same ownership problem plus the 21-title blacklist. Fallback only.
- **ARM7-timed IPC** (an ARM7 VCOUNT IRQ sends an IPC sync and the ARM9 does the RMW): it moves the problem to the ARM7's LYC, which NitroSDK's V-alarm/touch-panel sampling may own. It adds IPC latency, and the IPC sync value is shared with nds-bootstrap commands (`cardengine.c:1645-1700`). Not better than NDMA timing.

**Verdict:** NDMA as a private timer (2.1) replaces all of these.

### 2.10 DSi-only hardware

| Feature | Use for notices | Verdict |
|---|---|---|
| NDMA 0-3 (SCFG_EXT9.16, already on) | FIFO feeder (09h); H-Blank line counter (07h); **one-shot delay timer via BCNT interval** | Core of 2.1. melonDS doesn't model BCNT (`DSi_NDMA.cpp` has no interval logic), so the delay timer needs hardware (H2). |
| SCFG_EXT9 bit 8 (extended ARM9 IRQs) | NDMA IRQs (IE 28-31) | Set pre-lock in our bootloader. DSi-mode games already run with it (`main.arm9.c:416`). Needs the dispatcher patch. |
| SCFG_EXT9 bit 12 "Extended LCD circuit", bit 3 "Revised 2D engine" | Undocumented (`gbatek.txt:24295-24301`) | Nothing known that helps. Changing NITRO behaviour risks compatibility. Optional curiosity test (H11). |
| SCFG_EXT9 bit 13 "Extended VRAM" | Timing only (the in-game menu's VRAM boost) | No help. |
| NWRAM (DSi WRAM A/B/C) | Banner source with no main-RAM contention | nds-bootstrap already runs ARM9 code there (colour-LUT engine at `0x03732800`, tables at `0x0374B800`/`0x03770000`; `common/include/locations.h:103`). Find 16-32 KB free (map it), and test whether 09h accepts it (H5). |
| 16 MB RAM | Badges, fonts, a frame queue | Plenty. Already used (module at `0x02D80000`). |
| Camera interface / camera | None for display | — |
| **Camera LED** (BPTWL[31h]) | Second LED channel with hardware blink; 3DS-emulated | **New; build it** (section 2.11). |
| Backlight level (BPTWL[41h]) | Whole-screen dim pulse | Nonvolatile (`gbatek.txt:31608-31609`, "nonvolatile!"). Writing it touches the MCU's persistent store. **Reject** in the spirit of the "no persistent writes" rule. |
| Power LED colour (BPTWL[63h]) | Colour cue | DSi only (not 3DS). Prior concerns about low battery stand. Optional. |
| DSP + SNDEXCNT ratio | Audio chime | See 2.12. |

### 2.11 LED cues (fallback during-play cue)

Built today: Wi-Fi LED patterns (`cardengine.c:1893-1976`). Add the **camera LED**:
- steady while a challenge is primed (`31h=01h`);
- hardware blink while a leaderboard runs (`02h`);
- off otherwise.

The Wi-Fi LED keeps unlock and progress blinks. That splits the "what" across two LEDs, at one I2C write per state change, on the ARM7 outside IRQ where possible. Residual: about 0.2-0.4 ms of ARM7 I2C per write (OR's estimate, unmeasured), and no screen or game state.

**Verdict:** keep, and add the camera LED. It is the best rule-clean cue and is also available on 3DS. A camera LED that is on while the game doesn't use the camera can't confuse an NTR game, which has no camera access.

### 2.12 Sound cues

**DSP chime:**
- Needs SCFG_EXT9 bit 18 and SCFG_CLK bit 1 set before the lock, NWRAM for DSP code, and a Teak build.
- The chime is mixed in by SNDEXCNT ratio (`gbatek.txt:28062-28090`). Any DSP audio means the NITRO share drops to 7/8 or lower: −1.16 dB on the game's audio, either permanently or as a dip only during the chime (an audible artifact).
- Residual: a volume dip of 12.5% for about 0.5 s. Possibly a click at the ratio switch (H9).

**Spare NITRO sound channel:** game state (SND allocator). Reject.

**Verdict:** a possible audio fallback with a small, audible residual. Low priority (high effort, and a rule bend on audio).

### 2.13 Creative ideas, examined

| Idea | Finding | Verdict |
|---|---|---|
| Use the game's lag frames | On a lag frame the hardware shows the same registers and VRAM; nothing becomes free. The module can use the idle time it already measures (section 0 item 6). | No pixels gained. Its value is the slack gate. |
| Use fades | MASTER_BRIGHT applies after the selector, so a strip fades with the game. During a full fade nothing we draw on that engine shows. | Defer notices during fades; don't draw into them. |
| Black/white frames | See 2.3. | **Promising.** |
| Display mode 0 (off) or forced blank | Covered by 2.3. | Part of 2.3. |
| LCD swap (POWCNT1.15) | Game register. Swapping flips screens. | Reject. |
| Borrow a whole engine when POWCNT1 shows it off (bit 1 or bit 9) | Needs a POWCNT1 write (the game RMWs it; NitroSDK `GX_SetPower`), plus a VRAM bank. Rare in games. What the panel shows with engine B off is unknown (H10). | Niche. Only via 2.5's compare discipline. |
| 3D rear plane / one quad when the game is 2D | The 3D output only reaches the screen through BG0 (DISPCNT bit 3, the game's layer) or capture. The rear plane needs texture slots 2/3 = banks C/D. | Reject. |
| Letterbox detection (draw over black bars) | Can't see the composed image without capture (a bank, plus latency). | Reject. |
| Strip at the bottom | FIFO must be fed from line 0 (96 KB a frame, about 9-10% bus), and the mode must be *entered* mid-frame. Exit at VBlank is free. Per-title choice for top-HUD games. | Option for 2.1 at higher bus cost. Measure in H3. |
| ARM7 drawing | The ARM7 can't reach engine registers. It sees VRAM only for C/D mapped to the ARM7. | No. |
| NDMA-only switch-back (no IRQ) | 32-bit writes only, so whole-DISPCNT clobber (OR 4a, measured). There is no DMA arithmetic to do an RMW. | Proven dead. |
| `GX_DispOff` patch | Removes the main leak path of 2.1 and 2.3. | **Adopt** (boot-time). |
| Feed the FIFO with fill-data for blank rows | One channel can't switch from fill to memory mid-transfer, and two channels on the same trigger would double-feed. | Not useful. |

---

## 3. Ranking

| # | Mechanism | Graphical | Strict zero-cost? | Smallest residual | Universality | Recommendation |
|---|---|---|---|---|---|---|
| 1 | Pre-boot placard + menu "Now" page (built/planned) | yes (outside play) | yes | none (the menu is player-requested) | all | keep |
| 2 | LEDs: Wi-Fi + **camera LED** | no | yes | ARM7 I2C time per write | DSi all; camera LED also 3DS | build the camera-LED half now |
| 3 | **2.1 FIFO strip with NDMA timer** | yes, top band | no | DISPCNT byte 2 = 3 for about N+2 lines a frame; 0.1-0.2% CPU; 1.8-4.5% bus while shown; boot patch of `OS_IrqHandler` (+ `GX_DispOff`) | broad (M1 decides) | hardware experiment H1-H8; then opt-in + allow list + slack gate |
| 4 | **2.3 Black-frame card** | yes, large | no | as 3, plus a possible 1-frame hide of a fade-in; load-time bus | depends on M3 | prototype with 3 |
| 5 | 2.2 VRAM-display strip | yes | no | as 3 minus bus, plus writes into a free A-D bank | narrow | only if H1 fails |
| 6 | 2.5 Blank-sub-screen text (engine B) | yes, bottom | no | game registers/palette hold our values; 1-frame overdraw on wake | narrow-medium | fallback, allow list |
| 7 | 2.12 DSP chime | audio | no | 12.5% volume dip during the chime | all DSi | later, if wanted |
| 8 | 2.4 OBJ/BG borrowing | yes | no | proven sprite/tile corruption on a wrong guess | per title | trace-built opt-in only |
| — | Capture compositing, paused takeover, brightness/windows/blend, 3D quad, LCD swap, backlight | — | no | +1 frame latency / pause / game fades broken / nonvolatile writes | — | reject |
| (3DS) | TwlBg overlay (OR section 6) | yes, full | yes on the DS side | none on the DS side | all | the real answer on 3DS |

Bottom line: on a DSi, no on-screen mechanism meets "zero gameplay cost" to the letter, because every pixel route crosses a game-owned display register. The smallest residual found is the 2.1 design: one byte of DISPCNT for about N+2 lines, three short IRQs, and gated bus time, with the game's own VCOUNT, H-Blank, timers, VRAM, OAM and palettes untouched. Whether that is acceptable is the owner's call. Everything else in the graphical column is strictly worse.

---

## 4. Experiments

All pass/fail marks are set here, before any run. Log to SD (hardware) or to the runner log (melonDS), and write up in TESTING.md.

### melonDS (desk; another session owns the emulator, so queue these)

Known melonDS inaccuracies that limit these runs:
- NDMA 09h mapping is GBATEK's guess (`DSi.cpp:52-60`);
- NDMA BCNT interval isn't modelled;
- SCFG_EXT9 bit 8 doesn't gate IE 28-31;
- DMA stalls the whole ARM9 (no cache/TCM overlap), so bus cost is a worst case;
- rendering is per line (no mid-line latching);
- VRAMCNT is readable (hardware unknown);
- BPTWL has no LEDs.

- **M1. Library ownership survey.** Run each RA-set DS game for 3 x 5 minutes (boot, menus, play). Per frame, log:
  - DISPCNT A/B writes as (PC, VCOUNT, old, new), flagging mode-bit writes and writes during lines 261-31;
  - DISPCAPCNT enable/source;
  - legacy DMA start mode 4;
  - ARM9/ARM7 DISPSTAT writes that change bit 4/5 or LYC, and IE writes that would clear bits 28-31 (absolute writes);
  - VRAMCNT A-I;
  - POWCNT1;
  - MASTER_BRIGHT A/B;
  - engine-B layer bits;
  - calls to `GX_DispOff`/`GX_DispOn` (by signature PC).

  **Pass for 2.1:** at least 80% of titles show (no FIFO/capture-B-FIFO use) AND (no `GX_DispOff` call during lines 261..31, or a `GX_DispOff` signature found) AND (no absolute IE write that clears bits 28-31, or the irq-enable hook covers it). Below 50% means stop 2.1. 50-80% is the owner's call.
- **M2. Signature census:** `OS_IrqHandler` and `GX_DispOff` signatures across the same ROM set. **Pass:** both found in at least 90% of titles.
- **M3. Black-stretch census** (from M1 logs): stretches of at least 30 frames where engine A is forced-blank, mode 0 or backdrop-only. **Pass (worth building 2.3):** the median title has at least 1 per 5 minutes of play.
- **M4. Bank census** (from M1): fraction of play frames with a bank A-D disabled or LCDC and never written by the CPU in the session. Also H/I disabled for 2.5. **Pass for 2.2/2.5:** at least 30% of titles have such a bank for at least 90% of play frames.
- **M5. Idle-slack distribution** (the module already measures idle): per title, the fraction of frames with at least 5% idle. **Pass for the slack gate to allow notices:** at least 90% of play frames in at least 80% of titles.
- **M6. Patch the melonDS fork** so SCFG_EXT9 bit 8 gates IE/IF 28-31 and NDMA respects BCNT intervals. This turns OR's false-pass risk into a real check. **Pass:** OR's `overlay-fifo` S9 scenario fails without bit 8 and passes with it.
- **M7. End-to-end 2.1 in melonDS** with Kirby, NSMB, Pokemon Platinum and Contra 4, a forced notice every 5 s for 10 minutes. **Pass:**
  - 0 pixel differences below line N+M versus a notice-off run with the same inputs;
  - lag-frame count identical;
  - `GXi_DispMode` never reads 3.

### DSi XL hardware (homebrew test ROMs unless noted)

- **H1. Does NDMA 09h feed the FIFO?** (OR test 1, kept.) **Pass:** banner on lines 0-15 and the game pattern from line 16-18, photographed.
- **H2. NDMA as a private timer.** Start an NDMA immediately with BCNT intervals (prescaler 0-3) and record VCOUNT plus a free-running counter at its IRQ (bit 8 set). **Pass:** the IRQ fires within ±1 line of target across 1000 trials at each of 3 delays, and the measured interval matches BCNT to within 1%.
- **H3. Mid-frame mode switch.** `STRB` to `0x04000002` from an NDMA-timed IRQ at line N-1. **Pass:**
  - the 3→1 edge is clean (no torn line) in 600 photographed frames;
  - DISPCNT bytes 0, 1 and 3 read back unchanged after the STRB;
  - a bottom-strip variant (enter mode 3 at 192-N) also shows clean.
- **H4. VRAMCNT readback.** Write a set of values (MST/OFS/enable) to each of A-I and read them back as 8/16/32-bit. **Pass (readable):** reads equal writes for all 9 banks. Otherwise GBATEK holds and 2.2/2.5 detection falls back to traces.
- **H5. FIFO source in NWRAM vs main RAM.** **Pass:** the banner is correct from NWRAM. Bus cost (a main-RAM busy loop on the ARM9, timed with the strip on and off) is under 0.5% of a frame for NWRAM and recorded for main RAM. The threshold is the owner's; 0.5% is the proposal.
- **H6. Underrun and starvation.** (OR tests 3-4.) A 128-word NDMA1 copy plus a legacy DMA3 burst during the strip. **Pass:** no banner pixel errors with the FIFO on NDMA0. Record what underrun looks like (no pass/fail).
- **H7. Dispatcher patch on real games** (real nds-bootstrap chain, SCFG bit 8 set before lock): Kirby, Pokemon Platinum, NSMB and Contra 4, 30 minutes each, forced notices. **Pass:**
  - no crash;
  - lag count identical to stock;
  - no visual difference outside the strip in 240 fps capture;
  - the IRQ count log shows 3 module IRQs on every shown frame and 0 otherwise.
- **H8. Late arming.** Arm the FIFO NDMA at line 261 instead of VBlank. **Pass:** the banner starts at line 0 on every frame (600 frames).
- **H9. Camera LED.** Write `31h` = 1, 2 and 0 from CE7 outside IRQ. Measure I2C time with the ARM7 timer, and film the LED. **Pass:**
  - the LED responds;
  - the hardware blink is visible;
  - the I2C write is under 0.5 ms;
  - the Atheros stays associated (ping) for 10 minutes of toggling;
  - no audio glitch.

  Repeat on the New 3DS XL: the camera LED should respond.
- **H10. Engine-B power-off appearance** (POWCNT1 bit 9 = 0). Record what the sub LCD shows (no pass/fail; informs 2.13).
- **H11. SCFG_EXT9 bits 3/12 curiosity.** Toggle each in the bootloader pre-lock with a test pattern. Record any change (no pass/fail). Never ship a change from this without a full compatibility pass.
- **H12. DSP ratio switch click.** Toggle SNDEXCNT ratio between 8 and 7 during a sine played by the NITRO mixer, recorded from the headphone jack. **Pass:** no click above −60 dBFS.

---

## 5. Open questions for the owner

1. Is "one byte of DISPCNT reads 3 for about N+2 lines a frame, plus a boot-time patch of `OS_IrqHandler`" an acceptable residual for an on-screen strip? This is the floor; nothing graphical on a DSi gets lower.
2. Is a load-screen bus cost acceptable for the black-frame card (2.3), given that it can lengthen loads slightly?
3. Is the bottom-screen fallback (2.5) wanted, given that it holds game registers and palette entries for the length of a notice, with compare/restore?
4. Should the camera LED take challenge and leaderboard state now? It needs no hardware unknowns beyond H9.
