# DScheevos architecture

How DScheevos runs RetroAchievements inside DS games on a DSi (and 3DS in DSi mode) without emulation. This is the working design; the open questions at the end still need answers before parts of it are final.

## Pieces

| Repo | What it holds |
|---|---|
| `skierrz/dscheevos` (this repo) | All DScheevos code, docs, tests and tools |
| `skierrz/nds-bootstrap` | nds-bootstrap with hook points only: small calls into DScheevos, no DScheevos logic |
| `skierrz/melonDS` (`dscheevos` branch) | Test bench only: headless runner, cache timing model, Wi-Fi through libslirp |
| `RetroAchievements/rcheevos` | Stock upstream today, trimmed through build settings. Patches only where results are proven identical to stock, kept minimal and carried on our fork, never sent upstream (`docs/rcheevos-pin.md`) |

Users install one release: a DScheevos build of nds-bootstrap plus a `_nds/dscheevos/` folder on the SD card. TWiLight Menu++ launches games through nds-bootstrap as usual.

## Session start (before the game runs)

The game isn't running yet, so this part has the whole machine.

1. The DScheevos launcher logs in, hashes the ROM with rcheevos' DS hash (header, ARM9 and ARM7 binaries, icon), fetches the achievement set and starts a session. Hardcore requires being online here.
2. It writes the replies to `_nds/dscheevos/session.bin`, tagged with the ROM's path (layout: `dscm_boot_t` in `dscheevos/module/include/dscheevos_module.h`; `tools/mksession` makes the same file offline for tests).
3. nds-bootstrap's front end finds a `session.bin` for the ROM it is booting, written by the launcher less than 10 minutes earlier by the console clock, for a ROM whose RetroAchievements hash (rcheevos' own code, run again here) is the session's, renames it to `session.used` so it can't be used twice, and passes the module's and the session's clusters to the bootloader. The bootloader zeroes `session.used` once the session is in RAM.
4. The bootloader loads `dscheevos9.bin` and the session into the reserved RAM, patches the game's `OS_Halt`, and switches on the card engine hooks. Its ARM9 then calls the module's init, which parses the set (rcheevos load) on the module's own stack.
5. The game boots.

Measured cost of step 4's parse: 0.85–1.4 s for a 60-achievement set and 14–27 s for a 250-achievement set at 67 MHz (see `docs/rcheevos-arm9-footprint.md` and `tests/do-frame-timing`).

## Every frame

```
ARM7 VBlank IRQ (nds-bootstrap CE7, existing handler)
  └─ vblank_count++ in the module header (no IPC message, nothing on the ARM9)

ARM9 idle: the game's idle thread calls OS_Halt (SDK wait for interrupt)
  └─ patched to jump to header->halt (nothing pushed on the game's stack;
     module stack, IRQs on)
       └─ if vblank_count moved: queue the snapshot taken at the last halt
          for each frame since (vblank_poll; also right before every copy)
       └─ evaluate every waiting snapshot in order (rc_client_do_frame)
       └─ network
       └─ IRQs masked: copy the bytes rcheevos needs (the snapshot for the
          next VBlank); stop at once if an IRQ is waiting
       └─ then wait for an interrupt, as OS_Halt did
```

- **Snapshot when the game halts.** The game's RAM is copied when its idle thread halts, i.e. when the game has finished its frame's work, and the VBlank only counts the frame: the ARM7 bumps a counter, and the idle thread reads it (`vblank_poll`) at each wake and right before every copy, so the copy taken at the last halt before the count moved becomes that frame's. Nothing of DScheevos runs in the game's interrupts or on its stacks (read-only rule, 2026-10-13; until then the ARM7 sent IPC sync 0xA every frame and the ARM9's IPC IRQ queued the frame on the game's IRQ stack). So achievement logic sees one consistent frame. The copy runs with IRQs masked in chunks of 8 words or chain steps; between chunks, a waiting IRQ runs at once and the copy goes on, unless two scanlines or more went by (a game thread probably ran) or the ARM7's VBlank count moved: then it starts over, leaving nothing behind (the pointer chains' prior/changed state is saved first and put back). The last copy of a pass and the halt are done with IRQs masked from one to the other, so no game code runs between them. Each halt replaces the copy from the one before, since the game may have run in between. A VBlank with no copy since the last one (the game was still busy: a lag frame) is evaluated with the last copy's memory, as a repeat, so hit counts still advance once per frame. RetroArch would see the game's half-updated memory there; DScheevos sees the last finished frame. The very first frame of a session (or after a reset) is copied when its VBlank is polled, once. The ARM9 does the copy (not the ARM7) because only the ARM9 sees what's in its own data cache. Earlier builds copied at the VBlank inside the interrupt, which cost the game 10–35% of a frame in melonDS (`tests/perf/AB.md`).
- **A counter read in idle time.** The ARM9 reads the ARM7's counter past the data cache (through the uncached 0x0C000000 mirror, or by dropping the line where a game caches the mirror). VBlanks that pass with no idle time (a load) are queued one frame each at the next poll, as repeats of the last copy; the commits after a long load run with IRQs on (`dsc_drain`).
- **What gets copied.** Not all 4 MB. When the game loads, DScheevos lists every address the set reads directly. Each copy takes every aligned word that holds one of them, straight from main RAM and DTCM (the module gives the core their addresses), with a list of loads built when the address list changes. Pointer chains (AddAddress) are copied too: each copy runs them on live memory, keeping the bytes each pointer lands on. The usual DS shape ("pointer & 0x1FFFFFF, plus an offset") runs in one step; rare shapes go through rcheevos' own code, and the tests check the core's steps against it. So a queued frame is checked with memory from its own VBlank, even when a pointer moved before idle time came. For the synthetic typical set a copy costs about 12k ARM instructions (17k when it is compared with a queued frame for the repeat check), all in idle time; the VBlank IRQ's part is about 60 instructions. `tests/perf/run.sh --gate` holds both there. A chain that uses Remember/Recall falls back to a live read during idle time, which may be from a later frame; `live_reads` counts those. `tests/games` checks this by holding idle time back 600 frames and comparing with rcheevos reading live memory every frame.
- **Reads that cross the end of a region.** A read not entirely inside main RAM or DTCM fails whole, live and from a snapshot alike. rc_client then disables an achievement whose direct read fails, as it does in RetroArch (whose DS memory map also returns short reads there). A pointer chain that lands across the end reads 0; RetroArch would return the bytes before the end. Sets rarely read there.
- **Work in idle time only.** `OS_Halt` runs in NitroSDK's idle thread, the lowest priority. The module works only when the caller is the idle thread's loop (the instruction after its call branches back to it); a game thread that halts until its own IRQ comes gets the stock halt. Nor does it work while the game's MPU makes its data read-only: NitroSDK's `CTRDG_Init` (from `OS_Init`) sets the GBA-slot protection region, which holds the `0x0C000000` mirror the module's data goes through, read-only for data until the ARM7 answers, and `CTRDG_Enable(FALSE)` can do it at any time. Every halt first reads that region's data permissions (CP15 c5; the region is found once from c6) and takes the stock halt unless they allow a privileged write; Professor Layton and Peggle idle inside that window and froze before the check (research: optimize/freeze.md). The in-game menu's flush waits for a later safe point then, and a reset writes its words through the `0x02000000` view of the same RAM. Idle time is shared out: each pass evaluates at most 4 frames, then the network, then copies again and halts; the next IRQ brings the next pass. (An idle loop that never halted stalled Kirby Super Star Ultra's streaming in melonDS.) The idle thread wakes at every IRQ, 20-40 times a frame in Kirby Super Star Ultra: only the first wake after a VBlank, or a wake with work waiting (frames to evaluate, answers, a reset, the network in the middle of a request), runs a full pass. Every other wake only copies and halts, deciding that from one cache line, so it evicts as few of the game's cache lines as it can. Frames from the access point while nothing is in flight wait for the once-a-frame network pass. `dscm_idle_step()` is the full pass on its own, for callers other than `OS_Halt`; a lock keeps a pass that a game thread parked mid-way the only one in the core.
- **Wi-Fi never next to a ROM read.** The ARM7 SDIO shim skips its poll while nds-bootstrap's card engine has an SD read in flight: in melonDS the two together made the reads fail and Kirby hung. NitroSDK switches threads when an IRQ returns, so the moment a game thread wakes (VBlank, card, sound) it takes the CPU back and DScheevos' work pauses mid-frame. It never delays the game.
- **A queue, not a fixed pair of buffers.** Snapshots wait in a ring (up to 1024 frames, sharing 256 KB). Each frame takes only the bytes the set watches, typically a few hundred. Games can go seconds without idle time: Kirby Super Star Ultra gives none while it formats a new save. Evaluation catches up afterwards, in order. Only if the ring fills is a frame dropped and counted as `frames_dropped`. Hit counts would then differ from RetroArch's, so the in-game menu shows it.
- **Resets.** nds-bootstrap's reset (the in-game menu's "Reset Game", or a soft reset) calls `header->reset`. Achievement state resets in the next idle pass, as an emulator calls `rc_client_reset`. Frames queued from before the reset are discarded.

## Memory

DScheevos uses 2 MB at `0x02D80000`–`0x02F80000` (the DSi's extra RAM, cached mirror). nds-bootstrap's ROM cache for DS games normally covers it, so the cache is cut to end at `0x0CD80000` (the same RAM through the uncached mirror); ROMs that loaded fully into RAM within about 2.4 MB of the old limit now use cache mode. Games with a preload entry (ROM ranges nds-bootstrap loads into the cache at boot) that fit only without the cut keep as much of it as fits (`dscheevosTrimPreload`), instead of losing all of it: without that, Mario Kart DS read its 10 MB preload from SD during play and its loading screens got longer (tests/perf/AB.md). 40 of nds-bootstrap's 140 DSi preload entries are affected; they keep 83-99% of it. The cut costs 159 of the cache's 760 16 KB slots (0x27C000, `DSCHEEVOS_CACHE_RESERVE`) in every layout. Before choosing ROM-in-RAM or a preload size, nds-bootstrap builds the ROM map it would use and checks that every piece ends by `0x0CD80000` (`dscheevosRomMapFits`); a ROM that doesn't fit beside the module runs in cache mode instead. The module and session are loaded after every boot-time load into the cache area (ROM-in-RAM, the AP-fix overlays, which pass through `0x0C800000`-`0x0CF00000` and are zeroed there, and the preload), and before the page file saves the ARM9 binary with the OS_Halt patch.

| Range | Use |
|---|---|
| `0x02D80000` + 512 KB | module image: header, code (rcheevos, lwIP, mbedTLS), data, bss (~296 KB + 107 KB today); the heap's low piece after it |
| `0x02E00000` + 1 KB | session header (`dscm_boot_t`) |
| `0x02E00400` + 256 KB | snapshot arena. Until the set is loaded it holds the rest of the session file (server replies, optional extra CA, DER) and rcheevos' parse of the set: the arena isn't written before the first frame (`module/src/heap.h`) |
| `0x02E40400` – `0x02F7D000` | heap's high piece: rcheevos game data, address tables, TLS (~1.2 MB) |
| `0x02F7D000` + 4 KB | spare (was the VBlank call's IRQ stack) |
| `0x02F7E000` + 8 KB | idle and init stack (`header->halt`, `header->init`) |

The set's raw JSON and its parsed copy cost no heap, so the load peak equals the steady state (Kirby Super Star Ultra: 293 KB of heap at either, against 604 KB at the load peak before). newlib's malloc takes the two heap pieces from `_sbrk` as one heap with a gap.

**Big sets.** A session file bigger than the arena (a set reply over ~255 KB: Pokemon Platinum's is 311 KB) runs on from the arena into the heap's high piece; nds-bootstrap reads it there whole (up to `DSCM_SESSION_MAX` = `DSCHEEVOS_SESSION_MAX`, 384 KB). Before the first allocation the module moves the replies and CA to the top of the high piece (the header stays), stops the heap below them while the set loads, lends the set reply in place from there and gives the arena to rcheevos' parse. Once rc_api has parsed the set (it copies every string it keeps), the set reply's bytes go back to the heap, before rc_client builds its game data; the start-session reply and the CA stay until the module has read them. The game loses nothing: the module region is still 2 MB. Peak memory while loading is then about rc_api's parse plus rcheevos' game data: Platinum peaks at 946 KB of the high piece's 1,297 KB (1,258 KB before the early release). Pearl Rev 5 (409 KB), SoulSilver (612 KB) and TWEWY (1.1 MB) don't fit because of rcheevos' game data itself (Pearl 1.30 MB, SoulSilver 1.63 MB, TWEWY over 4 MB: 48-byte conditions), not the reply, so 384 KB keeps the launcher refusing them up front: a set reply over the limit isn't even parsed (launcher.log: "session: set reply N bytes, over the ...-byte limit"; Animal Crossing: Wild World's 2.4 MB set crashed the launcher's parse before), a session file over it is refused when written ("session: N bytes, over the limit"), and the game then boots without achievements. `tests/perf/fitmem.sh` measures any set's need without the emulator. research/optimize/bigset.md and fitmem.md have the numbers.

Code runs through the cached `0x02xxxxxx` view. Data goes through the `0x0C000000` mirror (`module.ld`, `DSCM_DATA_MIRROR`), which most games' MPUs leave uncached, so it never evicts the game's cache lines: statics always; the heap (rcheevos' game data), the snapshot arena and the core's state only for sets up to a 160 KB set reply (`DSCM_HEAP_UNCACHED_MAX`), because uncached evaluation is slower (Kirby's rcheevos +23% cycles); bigger sets keep them cached. 160 KB covers the biggest sets measured to keep every frame evaluated uncached (Pokemon Black, Mario Kart DS; research/optimize/opt-heavy.md): cached, Black's evaluation streams ~750 lines a frame through the 4 KB D-cache and cost the game ~45k cycles a frame more. Constants and the stack stay cached. Pointers the ARM7 reads are physical; the in-game menu reads the module's own view. SDK5 games map the mirror cached: nothing changes there. Kirby Super Star Ultra's game cost fell from 4.9% to 1.9% of a frame (research/optimize/opt-gamecost.md). Only DS (NTR-mode) games booted from SD use it: DSi-enhanced games in DSi mode use all 16 MB.

## Unlocks during play

rc_client raises an unlock inside `do_frame` and hands DScheevos a request. The module sends it over the Wi-Fi connection the launcher made, in idle time, never inside an interrupt.

```
launcher (BlocksDS DSWiFi, DSi Wi-Fi chip, WPA2)
  └─ associated; ARM7 freezes DSWiFi without telling the chip (no disconnect)
       └─ nds-bootloader → nds-bootstrap → game: the chip stays associated

game
  ARM7 VBlank (CE7): SDIO shim moves raw HTC frames between the chip's
                     mailbox and a ring in module RAM (dscm_shim.h)
  ARM9 idle (module): Ethernet ↔ HTC/WMI framing (net/link.c), lwIP
                     (NO_SYS), HTTPS with mbedTLS (net/https.c): POSTs
                     the queued requests, replies go back to rcheevos
```

- **The ARM7 shim is a dumb pipe:** it polls the chip once per VBlank, moves at most one frame each way, and does no protocol work. It costs at most about 6 scanlines when it moves a frame, under 1 when idle. Errors back it off for a second; after 5 failures in a row it stops.
- **No Wi-Fi keys on the SD card:** the session file carries the MAC address, IP address, gateway, DNS server, and the access point's name, BSSID, channel and ciphers. To start with, the chip keeps using the keys it got at association.
- **Rejoining (net/link.c, net/wpa.c):** if the access point drops us, the module waits (1 s, doubling up to 30 s) and joins again with `WMI_CONNECT`. On WPA2 it then runs the 4-way handshake itself. It needs the network's PMK for that, which the console keeps with its saved Wi-Fi settings (DSi slots 4–6 in the firmware flash): nds-bootstrap reads it, read-only, into the session in RAM before the game starts, and the module keeps it in its own RAM only. The launcher's keys are never known to the module, so the first group rekey (often hourly) also costs a rejoin, about a second offline; after that the module answers group rekeys in place. WEP networks and WPA (not WPA2) don't rejoin.
- **The supplicant** handles only key descriptor version 2 (CCMP pairwise, as the launcher's DSWiFi does), checks every MIC and replay counter, and is tested on the host against a real captured handshake (Wireshark's wpa-Induction sample) and a simulated access point.
- **In-game menu RAM dump** is hidden during DScheevos sessions: the dump would write the RA login token and the PMK to the SD card.
- **GPIO_WIFI:** stock nds-bootstrap switches every DS game to the DS-mode Wi-Fi (GPIO_WIFI bit 8), which on DWM-W024 boards turns off the DSi chip. With DScheevos it stays on the DSi chip. CE7 switches to DS-mode Wi-Fi only while the game is using it (POWCNT2 bit 1 and the DS Wi-Fi's clock on), and sending pauses meanwhile.
- **TLS:** mbedTLS with the launcher's config and root CAs. One keep-alive connection; reconnects resume the TLS session, so only the first handshake does the key exchange. That first handshake measured 5.5 s of wall time in melonDS with Kirby running. The handshake advances one step per idle pass, so achievement evaluation keeps up in between.
- **Measured in melonDS** (Kirby Super Star Ultra, HTTPS to the mock server):
  - four unlocks and a rich-presence ping reached the server during play
  - all 3585 frames evaluated, none dropped
  - the in-game menu shows "Online" and the unlocks sent

Also measured in melonDS: the access point dropping the console mid-game (`WIFIDROP` in the runner script), once and twice, including during a send. The module rejoined each time and every unlock still reached the server; the in-game menu counts the rejoins. nds-bootstrap read a WPA2 PMK from the firmware's saved settings into the module (runner `--wpa-ap`), and no copy stayed in the session.

Not covered by melonDS, and so untested until real hardware: the WPA2 handshake against a real access point (melonDS only offers an open one; the supplicant's own tests cover the protocol), group rekeys over the air, and real SDIO timing.

## Offline queue

Every unlock and leaderboard entry of the session goes into a journal in module RAM (`dscm_journal_t`), marked sent once the server answers. The module writes it through the data cache and cleans the lines at every step of an update (the core's `journal_sync` callback, in order: entry fields, then kind, then count), so RAM always holds a consistent copy. It doesn't use the `0x0C000000` "uncached" mirror: SDK5 games map it cached write-back (MPU region 3), so writes through it can stay in the cache. The same goes for the other words the ARM7 and ARM9 share in the module header (the VBlank count is read after dropping its line, the list pointer is cleaned after the store). nds-bootstrap's ARM7 copies it to `_nds/dscheevos/pending.bin` (4 KB, created by the launcher) only when the game is stopped: when the in-game menu opens (unless a ROM read is still in flight), before Reset and Exit, before the soft-reset and return combos, and before a reset the game asks for. Never from an interrupt while the game runs. At the next start the launcher sends what is unsent, with the time since each unlock (rcheevos' `seconds_since_unlock`), and clears the file; if it still can't, it keeps a copy in `unsent/` for later. Turning the console off between safe points loses the unsent entries since the last one.

**Time base.** The DS clock keeps local time with no time zone. The launcher reads the `Date` header of the server's replies and puts the clock's offset from UTC, to the quarter hour, in the session (`utc_offset_q`, `DSCM_BOOT_UTC_KNOWN`); the module then runs rc_client and the journal on UTC, as the server does, and the journal says so (`time_base`). Without the offset (no `Date` header, or a clock off by more than 14 hours) the times stay console time and are marked so. Queued entries are sent as seconds since they happened, like rc_client's own retries; a UTC journal whose "now" can't be measured goes as "just now" rather than hours off. `boot.unix_time` itself stays console time: nds-bootstrap's stale-session check compares it with the same clock. Host tests: `make -C dscheevos/launcher/tests test`.

The ARM7 writes with nds-bootstrap's `fileWrite`, which used to write back stale bytes when a write started mid-sector; that is fixed (from Bakkerrrs' retroachievements-ds-on-3ds).

## What the player sees

- **Game start.** Before the game boots, the top screen shows a card for 1.5 s (A, B or START skip it): the title, HARDCORE or Casual, unlocked/total achievements and points, and "Spectator: nothing is sent" when it applies. The screen is then cleared as stock, before the game's first frame.
- **Unlock.** The DSi's Wi-Fi LED blinks three times. It changes nothing on screen or in the sound, so it can't get in the game's way. The ARM7 reads the module's unlock count at VBlank and drives the LED over I2C, as nds-bootstrap already does for its ROM-read LED. The same LED blinks twice per leaderboard start, fail or submit and once for measured progress, and holds while a challenge is primed. A blink or the hold is the opposite of the LED's normal state, so it shows dark with wireless on. Not on 3DS, and not while nds-bootstrap's ROM-read LED option is on.
- **In-game menu (L+DOWN+SELECT).** Its "Achievements..." page opens on a summary (game, HARDCORE or Casual, unlocked/total achievements and points, this session's unlocks, network, what is waiting to be sent, active challenges and leaderboard trackers, frames checked/skipped), then lists every achievement with its state, points and measured progress; A shows one's description. The module rebuilds the list (`dscm_list_t`) in idle time when something changed and publishes it whole, so the menu never reads a half-built one.
- **Notices.** The module also keeps what rcheevos asks a client to show (`dscm_notices_t`: unlocks, progress, challenge indicators, leaderboard start/fail/submit/rank, trackers, offline/online, the mode at session start). Nothing draws them over the game; the LED cues above come from them. See `docs/overlay-research.md`.
- **Hardcore.** The in-game menu hides the RAM viewer and RAM dump (the dump is hidden in Casual too), and the refresh-rate option, whose rates below 60 Hz act as slow motion. nds-bootstrap's front end skips user cheats and `wideCheatData.bin`. AP-fix patches, which some games need to run, come only from nds-bootstrap's built-in `apfix.pck`: user files in `_nds/nds-bootstrap/apFix/` are ignored, since they would patch the game after the launcher hashed it. When a game has a user AP-fix file and no built-in fix, the launcher doesn't offer hardcore for it (Casual, which uses the file, or no achievements). nds-bootstrap has no save states.

## Measured so far

- `rc_client_do_frame` on a typical 60-achievement set: 10.6 ms at 67 MHz (63% of a frame), 7.7 ms at 133 MHz (46%), with the cache timing model. A 250-achievement set needs 2–3 frames per frame.
- So the CPU budget is the main risk. Many games may not leave that much idle time.
- Kirby Super Star Ultra (USA) through the DScheevos nds-bootstrap in melonDS: 1526 of 1531 VBlanks snapshotted and evaluated over 1800 frames, none dropped; the 5 missed were before the game turned on its interrupts. A 300-frame test achievement unlocked at frame 306.

## Open questions

1. **Not enough idle time.** Snapshots queue for up to 1024 frames (17 s), so busy stretches are fine as long as the game idles enough on average. A game that never does would fall behind for good. The answer then is cheaper evaluation (faster read path, cheaper memory layout) or marking the game unsupported.
2. **Games that never call `OS_Halt`.** Some games busy-wait; they give no idle time at all. Need to find how common this is. DScheevos turns itself off when it finds no `OS_Halt` (the bootstrap log says "DScheevos off: no OS_Halt"). Kirby Super Star Ultra and Pokémon Platinum both have one. Some games also have phases without it: during Kirby's attract-mode intro video the game's thread waits in nds-bootstrap's ARM9 card engine (streaming from SD) through the BIOS, so evaluation pauses, and after 17 s of that the queue is full and frames are skipped. Frames whose watched bytes match the frame before now queue as repeats of it, using no queue space; each repeat is still evaluated as its own frame. So a cutscene that leaves the watched memory alone can run as long as it likes. A set that watches something changing every frame, like a timer, still fills the queue after 17 s. Running the module inside that wait would cover that too, but it would delay every read, so it isn't done.
3. **DSi-enhanced games in DSi mode** use all 16 MB, leaving no free RAM. They run with achievements only in DS mode.
4. **Homebrew games** (like the homebrew titles with RetroAchievements sets) go through nds-bootstrap's separate homebrew path, not the retail card engine, and wait for VBlank through the BIOS (`swi 0x05`) instead of `OS_Halt`. They need their own hook points.

Settled since the first draft: WPA2 (the launcher uses the DSi's own Wi-Fi through BlocksDS DSWiFi; the module rejoins and rekeys itself) and pop-ups (unlocks blink the Wi-Fi LED; details are in the in-game menu, so nothing draws over the game).
