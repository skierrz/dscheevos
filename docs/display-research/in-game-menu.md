# The in-game menu (IGM) as a model for a DScheevos overlay

Research of 2026-10-12. Read-only. Nothing was built or run.

Paths are relative to `dscheevos/external/nds-bootstrap/retail/` (branch `dscheevos-perf`) unless they start with `dscheevos/`. The size figures come from `cardenginei/arm9_igm/build/cardenginei_arm9_igm.map`, built 2026-10-11 21:56, one minute after the last change to `dscheevosMenu.c`, so they match the current source. Time figures marked **est.** are estimates, not measurements.

## Summary

- **The menu takes over one display engine, not two.** It reprograms only engine B (the sub engine), forces engine B onto the bottom LCD, and leaves engine A, 3D, OAM and the main palettes alone. The top screen keeps showing the game's frozen engine-A picture.
- **The game is stopped, not shared.** Both CPUs sit inside the game's own IRQ handlers with IME=0 for the whole time the menu is open:
  - ARM9: inside its IPC-sync IRQ.
  - ARM7: inside its VBlank IRQ.
  - Every displayed frame is a frame the game does not run.
- **Entering and leaving costs about 10-16 frames (est.) on top of the time the player spends in the menu.** Most of it is SD paging:
  - On each of entry and exit: 40 KB written to SD and 40 KB read.
  - On entry, with DScheevos: also a journal write.
  - Handshakes wait whole frames.
  - The VRAM/register save and restore is only about 20 KB of copies, well under 2 ms.
- **A 1-2 s pop-up through this mechanism would freeze the game for about 70-136 frames per pop-up, and mute the audio.** No partial variant reaches zero gameplay cost. See section 3.
- **Free RAM for an overlay library:** apparently unused in DS-mode DScheevos sessions:
  - `0x02F80000-0x02F88000`: 32 KB.
  - `0x02F92000-0x02FD8000`: about 280 KB.

  This is found by searching for constants only. Confirm it with a fill-pattern watch.
- **Font:** 8x8, 1 bpp, 256 glyphs, 2 KB. It can be reused. The bootloader's placard already draws it into a pixel buffer.

---

## 1. How the menu takes over, and what it saves

### 1.1 Trigger path

1. **ARM7, inside the game's VBlank IRQ.** CE7 hooks the return address of the game's ARM7 VBlank handler (`cardenginei/arm7/source/card_engine_header.s:184-202`), so `myIrqHandlerVBlank` runs after the game's handler (`cardenginei/arm7/source/cardengine.c:2038`). It opens the menu when all of these hold (`cardengine.c:2141`):
   - the hotkey (L+DOWN+SELECT by default) is held;
   - `igmAccessible` is set;
   - `!wifiIrq`. That is, the game's own DS wireless is not active: `cardengine.c:2314-2321` checks POWCNT2 / `REG_WIFIIRQ`, with a 30-frame hysteresis.

   It then takes `saveMutex`, releases the DScheevos LED (`cardengine.c:2142-2147`), and calls `inGameMenu()` (`cardenginei/arm7/source/inGameMenu.c:60`).
2. **ARM7 `inGameMenu()`** does these steps in order:
   - Sets `sharedAddr[4]='MENU'` and sends `IPC_SendSync(0x9)` (`inGameMenu.c:62-64`).
   - Sets `REG_MASTER_VOLUME = 0` (`:65`).
   - Enters a critical section, so ARM7 IME=0 (`:66`).
   - Calls `loadInGameMenu()` (`:68`).
3. **ARM9, inside the game's IPC-sync IRQ.** CE9 hooks the game's IPC handler the same return-address way (`cardenginei/arm9/source/card_engine_header.s:637-656`). `myIrqHandlerIPC` (`cardenginei/arm9/source/cardengine.c:1619`) runs in this order:
   - the card-DMA continuation;
   - the DScheevos VBlank hook (`:1643-1647`);
   - on sync value 9, `inGameMenu((s32*)0)` (`:1714-1715`).
4. **CE9 `inGameMenu`** (`cardenginei/arm9/source/cardengine.c:1545-1600`) runs in this order:
   - `enterCriticalSection()`, so ARM9 IME=0.
   - Waits until the ARM7 raises and then drops `'IGML'`, the "menu code loaded" flag (`:1548-1555`).
   - Stores `sharedAddr` into the IGM header.
   - Calls the IGM at `INGAME_MENU_LOCATION + IGM_ENTRY` (`:1563-1565`).
   - On return, waits for the second `'IGML'` cycle, the unload (`:1567-1574`).
   - Resets if asked, then `leaveCriticalSection` (`:1599`).
5. **The IGM itself** is `inGameMenu(mainScreen, consoleModel, exRegs)` in `cardenginei/arm9_igm/source/inGameMenu.c:873`. The asm entry `igm_arm9` (`card_engine_header.s:52-58` in arm9_igm) only pushes registers r2-r11 and lr.
   - It has no stack of its own. The `__sp_*` symbols in `cardengine.ld.in:14-16` are unused, because there is no crt0.
   - So the IGM runs on the game's ARM9 IRQ stack, in the game's DTCM, below the game's SP. See 1.6.
6. **Keys.** The ARM9 IGM cannot read X/Y or the hinge, so the ARM7 loop writes the key state into `sharedAddr[5]` every frame (`arm7/inGameMenu.c:85-86`). The ARM9 side waits on `sharedAddr[5]=='REDY'` handshakes (`arm9_igm/inGameMenu.c:940`, `arm7/inGameMenu.c:71-80`).

### 1.2 How the game is stopped

| CPU | Where it sits | IME | What stops |
|---|---|---|---|
| ARM9 | inside the game's IPC IRQ dispatcher, which runs `myIrqHandlerIPC` | 0 | every game thread, every ARM9 IRQ (VBlank, timers, card, DMA), the game's IRQ-time code, and DScheevos' idle and VBlank work |
| ARM7 | inside the game's VBlank IRQ, which runs `myIrqHandlerVBlank` | 0 | the sound sequencer (timer IRQ), touch/RTC/PM polling, the game's Wi-Fi IRQs (the menu can't open while those are active anyway), and the DScheevos SDIO shim |

The hardware keeps running while the game is stopped:
- display scanout of both engines;
- the 3D engine re-rendering its last polygon list;
- sound channels;
- timers;
- repeating legacy DMAs (H-Blank and V-Blank start).

### 1.3 Everything saved and restored (`arm9_igm/source/inGameMenu.c`)

| State | Saved? | How | Lines |
|---|---|---|---|
| `DISPCNT_SUB` | yes, read and restored | then set to mode 0, BG3 only | 887, 900, 1117 |
| `BG0CNT-BG3CNT_SUB` | yes | BG0-2 set to 0; BG3 gets map base 15, tile base 0, 256x256 | 888-891, 901-904, 1118-1121 |
| `VRAMCNT_C` | yes, read | switched to LCD only if mapped to sub BG (`CR & 4`) | 893, 906-907, 1123 |
| `VRAMCNT_H` | yes, read | set to `ENABLE | SUB_BG` (0x06200000, 32 KB) | 894, 908, 1124 |
| `POWCNT1` | yes | swap bit forced on, so engine A goes to the top and engine B (the menu) to the bottom. On exit: restored, then the "main screen" option is re-applied | 896, 914, 1126-1131 |
| Engine B master brightness `0x0400106C` | yes | set to 0 | 898, 916, 1115 |
| `BG3HOFS/VOFS_SUB` | **no** | set to 0, not restored (write-only) | 910-911 |
| `MOSAIC_SUB`, `BLDCNT_SUB`, `BLDALPHA_SUB`, `BLDY_SUB` | **no** | set to 0, not restored; the comment says all are write-only | 917-920 |
| Sub BG map, 0x06207800, 0x600 B (map base 15) | yes, `bgMapBak` | copied after bank H is mapped, so it is bank H's content | 922, 1111 |
| Sub BG palette, 512 B | yes, `palBak` | filled with the menu palette | 925-929, 1112 |
| Sub BG tiles, 0x06200000, 8 KB (`sizeof(font)*4`) | yes, `bgBak` | 1 bpp font expanded to 4 bpp | 931-937, 1113 |
| ARM7 RAM viewer buffer, 0xC0 B | yes, `arm7RamBak` (RAM viewer only) | | 732, 869 |
| Engine A (DISPCNT, BG, OBJ, OAM, palettes, VRAM A/B/D-G/I, master brightness) | untouched | engine A keeps scanning the game's last frame | n/a |
| Sub OAM, sub OBJ palette, sub windows, ext palettes, BG2/3 affine | untouched | hidden by the new `DISPCNT_SUB` and restored with it | n/a |
| 3D (GXSTAT, geometry FIFO, `DISP3DCNT`), display capture | untouched | | n/a |
| IE / IF | untouched | IME is restored by `leaveCriticalSection` in CE9 and CE7 | ce9 `cardengine.c:1546,1599`; ce7 `inGameMenu.c:66,255` |
| DMA (legacy and NDMA) | untouched; nothing is stopped | the IGM copies with the CPU (`tonccpy`) | n/a |
| ARM7 master volume | set to 0 on entry, **set to 127 on exit, not restored** | `arm7/inGameMenu.c:65,256` |

Only the screenshot item changes more:
- It maps a whole bank to LCD and backs up 0x18000 bytes of it to `vramBak` before the capture, then restores the bank (`inGameMenu.c:281-397`; save `:361`, restore `:382-383`).
- It needs the ARM7 to first save 256 KB of ROM cache to the SD page file (`SSPP`, `arm7/cardengine.c:983-990`).

The IGM relies on reading `VRAMCNT` (`:893-894`, and the screenshot's bank search `:284-299`).
- GBATEK marks those registers write-only.
- melonDS returns their values (`dscheevos/tools/melonDS/src/NDS.cpp:2849-2858`), and upstream nds-bootstrap has shipped this code for years.
- So in practice they read back. `docs/overlay-research.md` §5 says "VRAMCNT is write-only"; that claim should be corrected or tested on hardware.

### 1.4 Where the save buffers live

The save buffers are static variables in the IGM's own `.bss`:
- `bgBak`: 8,192 B
- `bgMapBak`: 1,536 B
- `palBak`: 512 B
- `arm7RamBak`: 192 B

`.bss` spans `0x02F8DB48`-`0x02F90428` (0x28E0 = 10,464 B; map lines 276-306). That is inside the IGM window at `INGAME_MENU_LOCATION 0x02F88000` (`common/include/locations.h:61`), in DSi extra RAM.

The IGM window is not resident:
- **Entry**, `loadInGameMenu`: the ARM7 copies `0x02F88000-0x02F92000` (0xA000 = 40 KB) to `pageFile`+0xA000, then reads the menu image from `pageFile`+0 (`arm7/cardengine.c:1111-1118`).
- **Exit**, `unloadInGameMenu`: the reverse. It saves the image back, so its static variables persist, then restores the old 40 KB (`:1120-1130`).

Screenshot and manual buffers use `INGAME_MENU_EXT_LOCATION 0x026B8000`, 256 KB (`locations.h:58`):
- the BMP at +0 (0x18046 B);
- `vramBak` at +0x18200 (96 KB).

That area is inside the ROM cache, so it is first saved to `pageFile`+0x540000 (`arm7/cardengine.c:987,1010`).

### 1.5 How the game resumes "identically"

- **CPU state:** the IRQ entry and exit of each hook save r0-r12 and lr (`card_engine_header.s` in arm7 `:198-201` and arm9 `:651-654`), and the game's dispatcher saves the rest. IME is restored. The game continues at the interrupted instruction.
- **Display:** only the engine B subset listed above is restored.
- **Clock:** none of the time that passed is hidden. The game simply did not run.

### 1.6 What the game could notice

1. **BG3 scroll on the sub screen is left at 0** (`:910-911`). A game that set it once, such as a static scrolled bottom-screen BG3, shows a shifted layer until it next writes HOFS/VOFS.
2. **Sub-screen blending and mosaic stay off** (`:917-920`). `BLDCNT` and `BLDALPHA` are readable on DS (melonDS `GPU2D.cpp:212-214,242-243`), so the "write only, can't back up" comment is wrong for them. Only `MOSAIC` and `BLDY` really can't be read.
3. **Master volume is forced to 127 on exit** (`arm7/inGameMenu.c:256`), whatever the game had set.
4. **Lost IRQs:** N VBlanks while the menu is open become one pending IF bit. Timer overflow IRQs also collapse into one, so NitroSDK's `OS_GetTick` (TIMER0 overflow counting) and alarms lose time. The RTC jumps forward.
5. **Repeating legacy DMAs keep running.** For example, an H-Blank DMA into `BG_PALETTE_SUB` or the sub scroll registers would corrupt the menu, though not the game.
6. **The engine A picture moves screens** while the menu is open, for games that put engine A on the bottom. Games that alternate screens each frame with dual-screen 3D and capture freeze on one half.
7. **Stack:** the IGM, including the DScheevos page with its 101+33-byte buffers in `printWrapped` (`dscheevosMenu.c:69-71`), runs on the game's IRQ stack in DTCM. It writes dead stack below SP. That is harmless unless the game's IRQ stack is too small. This is unmeasured; worth a `-fstack-usage` check.
8. **The exit path re-applies the "main screen" option**, which can change the swap bit away from the game's own setting (`:1128-1131`).
9. **Reads of the DScheevos list and notices can tear.** The IPC IRQ can interrupt the module's idle work in the middle of an update. The list pointer is swapped whole (`dscheevos_hooks.h:34`), but notices and indicators are updated in place, so a menu page can show a half-written string. This is cosmetic.

## 2. Costs

### 2.1 Copy sizes on the ARM9

| Phase | Writes | Bytes |
|---|---|---|
| Backup | map 1,536 + palette 512 + tiles 8,192 | **10,240** |
| Draw setup | map clear 1,536 + palette fill 512 + font expand 8,192 (2,048 iterations) | 10,240 |
| Restore | map + palette + tiles | **10,240** |
| Registers | about 20 register writes | n/a |

At roughly 4-8 ARM9 cycles per VRAM word plus loop overhead, this is about 30-80 k cycles, or **0.5-1.2 ms each way (est.)**. It is not the dominant cost.

### 2.2 SD and handshakes, with frames at 16.7 ms

**Entry:**
- ARM7 `loadInGameMenu`: 40 KB SD write + 40 KB SD read. Est. 25-60 ms, assuming about 1-3 MB/s writes and 2-5 MB/s reads through nds-bootstrap's SDMMC; not measured.
- CE9 polls `'IGML'` once per frame at line 191 (`cardengine.c:1548-1555`): +1-2 frames.
- `'REDY'`: the ARM7 polls once per frame (`arm7/inGameMenu.c:71-80`): +1 frame.
- **DScheevos addition:** the ARM9 waits for the ARM7 key loop, then sends `'DSCJ'`, which writes the offline journal (1,620 B plus zero fill to 4 KB `pending.bin`; `arm9_igm/inGameMenu.c:949-963`, `arm7/inGameMenu.c:175-177`, `arm7/cardengine.c:2006-2031`): +1-2 frames + SD write time.
- **Entry total: about 5-9 frames (est.)** before the menu is drawn.

**Exit:**
- `EXIT` is seen by the ARM7 within 1 frame.
- `unloadInGameMenu` first waits a whole VBlank (`arm7/cardengine.c:1121-1122`), then 40 KB write + 40 KB read.
- CE9 waits for the `'IGML'` cycle: +1-2 frames.
- **Exit total: about 4-7 frames (est.).**

**Fixed overhead per open/close: about 10-16 frames (0.17-0.27 s, est.)**, plus 160 KB of SD traffic and 1 journal write.

For DScheevos sessions most of the paging is unnecessary. The 40 KB at `0x02F88000` is outside the ROM cache once the cache is cut at `0x0CD80000` (`bootloaderi/source/arm7/hook_arm9.c:372-374`). Upstream needs the backup only because the cache normally covers that address.

### 2.3 Lost frames

While the menu is open, every displayed frame is one in which the game does not advance. Opening for T seconds freezes the game for 60·T frames plus about 10-16 frames of overhead. The game itself only sees one late VBlank, but the player sees a freeze. That is acceptable only because the player asked for it, as a pause.

### 2.4 Audio

- The master volume is 0 for the whole time (`arm7/inGameMenu.c:65`).
- The ARM7 sequencer IRQ is blocked, so no new notes are played. Looping hardware channels keep cycling silently, and one-shot channels finish.
- On exit the volume is set to 127. That may differ from the game's own value, and resuming can click.

### 2.5 Wi-Fi

- **The game's DS wireless:** the menu can't open while it is active (`!wifiIrq`, `arm7/cardengine.c:2141`). If it could, the game's ARM7 Wi-Fi IRQs would be blocked and the link would drop.
- **The DScheevos connection:**
  - The ARM9 network stack (idle slices) and the ARM7 SDIO shim (ARM7 idle) are both frozen.
  - Short pauses are fine. A long one can miss a WPA2 group rekey or an AP inactivity timeout, which shows as `netRejoins` afterwards.
  - The journal is written on entry, so unlocks survive a Quit or Reset.

## 3. Could a 1-2 s pop-up use this mechanism?

The plain answer:
- A pause of N frames is N frames in which the game doesn't run. To the player that is N lag frames, and the audio is muted for the whole time.
- A 1-2 s pop-up on this path costs **60-120 frozen frames plus about 10-16 frames of entry and exit, so about 70-136 per pop-up**.
- That breaks "zero gameplay cost" outright. It is also an unrequested pause, which `docs/overlay-research.md` already rejects ("Auto-opening the menu").

| Variant | Lag cost | Touches game memory? | Meets zero cost? |
|---|---|---|---|
| A. Full IGM path for 1-2 s | 70-136 frozen frames; audio muted; SD writes during play | engine B regs, VRAM H/C mapping, sub palette (saved and restored) | **No.** Pause, mute, SD I/O |
| B. Resident "lite" takeover: no SD paging, no journal, sub engine only, a 32x2 tile strip, still with the game stopped | about 60-120 frozen frames (the takeover itself is under 1 ms; the time the pop-up is shown is the pause) | yes, as A | **No.** The display time is the pause |
| C. No pause: patch the sub engine every VBlank (save about 10 KB, draw, restore before the game writes) | 0.5-1.2 ms ×2 per frame inside the game's IRQ (about 6-14% of a frame), which can push busy games into lag | yes. The game writes and reads its VRAM, `BGxCNT` (read-modify-write in the SDK) and palettes at any time during the frame, so the pop-up leaks into game state or the game overwrites it | **No.** Breaks the rules on game RAM and VRAM, and can add lag |
| D. Only the sub engine, only a small VRAM region (for example 2 map rows + 40 glyph tiles, about 1.5 KB) | same as B if paused, same as C if not | yes, still game VRAM and palette entries | **No.** Smaller, but still the game's memory |
| E. Draw only during VBlank lines 192-262 and restore before line 0 | about 1 ms of the game's IRQ time | yes, briefly | **No.** And useless: nothing is scanned out during VBlank, so nothing is seen |
| F. Only during the game's own lag frames | Any CPU taken in a lag frame lengthens it, so the lag gets worse. The game presents at an arbitrary point, so restoring races it. A 1-frame (16 ms) pop-up is unreadable | yes | **No** |
| G. Only during fades, forced blank or loading | Master brightness at full black or white applies after compositing, so the pop-up is faded too and invisible. Forced blank shows white unless we clear the game's `DISPCNT` bit. Loading screens are exactly when games upload VRAM and stream from SD, and our SD paging competes with the game's reads | yes | **No** |
| H. Only when the sub engine is unused (`POWCNT1` bit 9 off or display off) | 0 CPU if done in idle, but turning engine B on is a readable `POWCNT` change, and a VRAM bank is still needed. Every bank belongs to the game, and the SDK reads back register state | yes | **No.** Could be argued as "residual only", but it is rare and still game state |

**Conclusion:** no variant of the IGM's "stop and take over" model meets zero gameplay cost. It is free only when the player asks for it, which is the existing menu and the "Now" page (option 2 of `docs/overlay-research.md`). The DSi's on-screen options stay as already ranked:
- the main-memory display FIFO strip (4c), with its known residuals;
- the LED cues;
- the pre-boot placard.

What the IGM study does add:
- **A cheaper menu.** In DScheevos sessions a resident or non-paged IGM would cut about 4-8 frames and 160 KB of SD I/O from every open and close. Any change to the IGM must keep the module's journal write on entry.
- **Fixes for an "identical resume" (A-B):** save `BLDCNT_SUB` and `BLDALPHA_SUB`, restore the game's master volume (read `SOUNDCNT` before muting), and re-write BG3 scroll from a known value. BG3 scroll can't be read back, so that one stays a residual.

## 4. Memory (DS-mode / NTR games, the only mode where DScheevos runs)

`dscheevosWanted()` requires `!dsiModeConfirmed && !isDSiWare` (`bootloaderi/source/arm7/dscheevos_hooks.c:34-36`). Addresses use the cached 0x02 alias; 0x0C + offset is the same RAM.

| Range | Size | Use | Evidence |
|---|---|---|---|
| `0x02000000-0x023FFFFF` | 4 MB | game main RAM | n/a |
| `0x023EC000` (or `0x02400000`) - `0x02D80000` | about 9.6 MB minus holes | ROM cache, or the ROM loaded whole into RAM. Cut at `0x0CD80000` for DScheevos. The skipped holes around `0x027C0000-0x02800000` are listed in the next rows | `locations.h:124-125,137`; `hook_arm9.c:357-374,436-444`; `main.arm7.c:860-892` |
| inside it: `0x026B8000` +256 KB | 256 KB | `INGAME_MENU_EXT` (screenshot and manual), used only in the menu, after a page-file backup | `locations.h:58`; `arm7/cardengine.c:987-1010` |
| inside it: `0x027D8000-0x027E6000` | 56 KB | ROM cache address, descriptor and counter tables | `locations.h:64-69` |
| inside it: `0x027FC000` | about 10-14 KB | CE9 (cardenginei_arm9), sharedAddr `0x027FFA0C`, exception stack `0x027FFC00`, NDS header `0x027FFE00` | `locations.h:6,96,105,117` |
| `0x02D80000-0x02E00000` | 512 KB | module image: header, code, data, bss (about 414 KB used per `docs/architecture.md`) | `dscheevos/dscheevos/module/include/dscheevos_module.h:36-46` |
| `0x02E00000-0x02E40000` | 256 KB | module session file | same |
| `0x02E40000-0x02F7D000` | 1,268 KB | module heap | same |
| `0x02F7D000-0x02F7E000` | 4 KB | module IRQ stack | same |
| `0x02F7E000-0x02F80000` | 8 KB | module idle and init stack | same |
| **`0x02F80000-0x02F88000`** | **32 KB** | **no NTR user found.** Used only by DSi-mode cards (`CARDENGINEI_ARM9_TWLSDK_LOCATION3`) and by an SDK5 DSi-mode workaround | `locations.h:100`; `main.arm7.c:1402` |
| `0x02F88000-0x02F92000` | 40 KB window | IGM: image 23,368 B (IgmText 0xF40 + code 0x4688 + rodata 0x4A4) + bss 10,464 B = **33,832 B** (to `0x02F90428`); linker limit 39 KB. Present only while the menu is open; otherwise the old contents, which round-trip through the SD page file and are preserved | `cardengine.ld.in:9`; map lines 71, 163, 247, 276-306; `arm7/cardengine.c:1111-1130` |
| **`0x02F92000-0x02FD8000`** | **about 280 KB** | **no NTR user found** | constant search over nds-bootstrap and dscheevos |
| `0x02FD8800`, `0x02FD9400`, `0x02FDC000-0x02FFC000` | n/a | DSi-mode CE9 and DSiWare cheats (not NTR); small ROM cache for `U`-prefixed NAND-save titles | `locations.h:86,99,132`; `hook_arm9.c:455-470` |
| `0x02FFD000-0x02FFFFFF` | 12 KB | TWLCFG copy, eMMC data, DSi header (`0x02FFE000`), SDK5 header area. Off limits | `main.arm7.c:342-346,1398-1406` |
| DSi WRAM | n/a | CE7 at `0x037E0400` (or `0x03020400`); colour-LUT CE9 at `0x03732800` | `locations.h:88,90,103` |

Notes on the free regions:
- **Boot clears them.** The bootloader zeroes `0x02F80000-0x02FFD7B0` (`main.arm7.c:342`), so nothing there survives from boot.
- **The candidates** are `0x02F80000-0x02F88000` (32 KB) and `0x02F92000-0x02FD8000` (about 280 KB).
- **They were found by searching for constants.** Before using them:
  - fill them with a pattern at boot and check it after 30 minutes in 3+ games, Kirby, NSMB, Pokemon Platinum and one `U` title;
  - keep a guard against `U` titles at `0x02FDC000+`.
- **The 40 KB IGM window can also hold overlay data** while the menu is closed, because the IGM's page swap restores it. The overlay just can't use it while the menu is open, which doesn't matter.

**Font in RAM:** at runtime the font exists only inside the page file (IgmText at page offset 0) and, while the menu is open, at `0x02F88000+offsetof(IgmText,font)`.

## 5. Font and drawing

**The font:**
- `IgmText.font[256*8]`: 8x8 glyphs, 1 bpp, one byte per row, bit 0 = leftmost pixel. 2,048 B (`common/include/igm_text.h:26`).
- It sits inside `IgmText` (0xF40 B with all the UI strings) at the start of the IGM image (`arm9_igm/source/card_engine_header.s:26-27`).
- Built at boot from two lz77 files (`retail/arm9/source/conf_sd.cpp:588-629`):
  - `fonts/ascii.lz77` (630 B, glyphs 0x00-0x7F);
  - a per-language extended set (410-696 B; Arabic, Chinese, Cyrillic, Greek, Hangul, Hebrew, Japanese, Vietnamese, extended Latin), glyphs 0x80-0xFF.

**The renderer:**
- A text BG on engine B: BG3, 32x24 map at map base 15, tiles at tile base 0. Each map entry is `char | palette<<12`.
- The font is expanded to 4 bpp at entry (`inGameMenu.c:932-937`).
- 6 text colours: palette n, colour 1 = `igmPal[n]`, background = `igmPal[6]`, or `[7]` for exceptions (`arm9_igm/card_engine_header.s:38-46`, `inGameMenu.c:926-929`).
- Helpers: `print`, `printCenter`, `printRight`, `printMsg` (wrapping, RTL-aware), `printChar`, `printDec`, `printHex` (`inGameMenu.c:87-170`).
- No sprites, no bitmaps, no badges.

**Reuse:**
- The glyph data and `fit()` (UTF-8 to `?`) carry over directly.
- The bootloader's placard already reads the ASCII half from the page file (`bootloaderi/source/arm7/dscheevos_hooks.c:129,184`) and draws it into an RGB555 buffer pixel by pixel (`:133-152`). That is the exact routine an RGB555 banner (overlay-research option 4) needs.
- The module can't call the IGM, because the IGM isn't resident. It should carry its own copy: 1-2 KB, GPL-3 like nds-bootstrap. Or the bootloader could copy `IgmText.font` into module RAM at init.
- The tile-BG renderer itself is not reusable in play, because it needs engine B's VRAM, palette and registers.

**How `dscheevosMenu.c` reads the module:**
- It reads module RAM directly, through the cached alias, on the same CPU as the module, so caches are coherent.
- It goes through pointers in the module header at `0x02D80000` (`dscheevos_hooks.h:24-39`):
  - `DSCHEEVOS_H_STATUS` +0x1C to `DScheevosStatus`;
  - `DSCHEEVOS_H_LIST` +0x2C to `DScheevosList` (`dscm_list_t`), 0 until loaded and swapped whole on rebuild;
  - `DSCHEEVOS_H_NOTICES` +0x30 to `DScheevosNotices` (`dscm_notices_t`).
- Checks, by structure:
  - Every pointer: `modulePtr()` checks it lies inside `[0x02D80000, 0x02F80000)` with room for the struct (`dscheevosMenu.c:20-26`).
  - Status: the header magic `DSCV` (`:28-33`).
  - List: magic `DSCL`, `entrySize == 176`, and `count` bounded by the module's end (`:36-43`).
  - Notices: magic `DSCN` (`:45-48`).
- The layouts are duplicated in `dscheevos_hooks.h:71-234`, with `_Static_assert`s on the sizes: entry 176, list 16, notice 116, notices 2,316, journal 1,620. They must match `dscheevos_module.h`.
- What it uses:
  - Summary: `recent[8]` (newest 4).
  - "Now active": `challenges[8]` and `trackers[8]`, `{id, text[24]}`.
  - List: entries' `title`, `points`, `state`, `flags`, `measured`, `description`.
  - It does **not** read the notice ring (`ring[16]`, `head`) or `indicatorsGeneration`. An overlay would use those as its event feed.
- No locking. See 1.6 item 9.
