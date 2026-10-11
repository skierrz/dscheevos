# DScheevos architecture

How DScheevos runs RetroAchievements inside DS games on a DSi (and 3DS in DSi mode) without emulation. This is the working design; the open questions at the end still need answers before parts of it are final.

## Pieces

| Repo | What it holds |
|---|---|
| `skierrz/dscheevos` (this repo) | All DScheevos code, docs, tests and tools |
| `skierrz/nds-bootstrap` | nds-bootstrap with hook points only: small calls into DScheevos, no DScheevos logic |
| `skierrz/melonDS` (`dscheevos` branch) | Test bench only: headless runner, cache timing model, Wi-Fi through libslirp |
| `RetroAchievements/rcheevos` | Used unmodified, trimmed through build settings |

Users install one release: a DScheevos build of nds-bootstrap plus a `_nds/dscheevos/` folder on the SD card. TWiLight Menu++ launches games through nds-bootstrap as usual.

## Session start (before the game runs)

The game isn't running yet, so this part has the whole machine.

1. The DScheevos launcher logs in, hashes the ROM with rcheevos' DS hash (header, ARM9 and ARM7 binaries, icon), fetches the achievement set and starts a session. Hardcore requires being online here.
2. It writes the replies to `_nds/dscheevos/session.bin`, tagged with the ROM's path (layout: `dscm_boot_t` in `dscheevos/module/include/dscheevos_module.h`; `tools/mksession` makes the same file offline for tests).
3. nds-bootstrap's front end finds a `session.bin` for the ROM it is booting, renames it to `session.used` so it can't be used twice, and passes the module's and the session's clusters to the bootloader.
4. The bootloader loads `dscheevos9.bin` and the session into the reserved RAM, patches the game's `OS_Halt`, and switches on the card engine hooks. Its ARM9 then calls the module's init, which parses the set (rcheevos load) on the module's own stack.
5. The game boots.

Measured cost of step 4's parse: 0.85–1.4 s for a 60-achievement set and 14–27 s for a 250-achievement set at 67 MHz (see `docs/rcheevos-arm9-footprint.md` and `tests/do-frame-timing`).

## Every frame

```
ARM7 VBlank IRQ (nds-bootstrap CE7, existing handler)
  └─ vblank_count++ in the module header, then IPC sync 0xA
       └─► ARM9 IPC IRQ (CE9, existing handler) calls header->vblank on every IPC IRQ
             └─ if vblank_count moved: copy the bytes rcheevos needs into a free slot
                (module IRQ stack; short)

ARM9 idle: the game's idle thread calls OS_Halt (SDK wait for interrupt)
  └─ patched to jump to header->halt (module stack, IRQs on)
       └─ evaluate every waiting snapshot in order (rc_client_do_frame)
       └─ then wait for an interrupt, as OS_Halt did
```

- **Snapshot at VBlank.** The game's RAM is copied at a fixed point in the frame, so achievement logic always sees one consistent frame. The ARM9 does the copy (not the ARM7) because only the ARM9 sees what's in its own data cache.
- **A counter, not just the sync value.** Other nds-bootstrap messages share the IPC sync register, so the ARM7 also bumps a counter in the module header. The ARM9 reads it past the data cache; a frame whose IRQ was overwritten is still snapshotted on the next IPC IRQ. VBlanks that pass with no IPC IRQ at all (ARM9 interrupts off for a whole frame) are counted as `frames_missed`.
- **What gets copied.** Not all 4 MB. When the game loads, DScheevos lists every address the set reads directly and copies those runs at each VBlank. Pointer chains (AddAddress) are copied too, and each VBlank runs the copies through rcheevos' own code on live memory, keeping the bytes each pointer lands on. So a queued frame is checked with memory from its own VBlank, even when a pointer moved before idle time came. A chain that uses Remember/Recall falls back to a live read during idle time, which may be from a later frame; `live_reads` counts those. `tests/games` checks this by holding idle time back 600 frames and comparing with rcheevos reading live memory every frame.
- **Work in idle time only.** `OS_Halt` runs in NitroSDK's idle thread, the lowest priority. NitroSDK switches threads when an IRQ returns, so the moment a game thread wakes (VBlank, card, sound) it takes the CPU back and DScheevos' work pauses mid-frame. It never delays the game.
- **A queue, not a fixed pair of buffers.** Snapshots wait in a ring (up to 1024 frames, sharing 256 KB). Each frame takes only the bytes the set watches, typically a few hundred. Games can go seconds without idle time: Kirby Super Star Ultra gives none while it formats a new save. Evaluation catches up afterwards, in order. Only if the ring fills is a frame dropped and counted as `frames_dropped`. Hit counts would then differ from RetroArch's, so the in-game menu shows it.
- **Resets.** nds-bootstrap's reset (the in-game menu's "Reset Game", or a soft reset) calls `header->reset`. Achievement state resets in the next idle pass, as an emulator calls `rc_client_reset`. Frames queued from before the reset are discarded.

## Memory

DScheevos uses 2 MB at `0x02D80000`–`0x02F80000` (the DSi's extra RAM, cached mirror). nds-bootstrap's ROM cache for DS games normally covers it, so the cache is cut to end at `0x0CD80000` (the same RAM through the uncached mirror); ROMs that loaded fully into RAM within about 2.4 MB of the old limit now use cache mode.

| Range | Use |
|---|---|
| `0x02D80000` + 512 KB | module image: header, code (rcheevos, lwIP, mbedTLS), data, bss (~274 KB + 90 KB today) |
| `0x02E00000` + 256 KB | session file: boot block, server replies, optional extra CA |
| `0x02E40000` – `0x02F7D000` | heap: rcheevos game data, address tables, snapshot ring, TLS (~1.2 MB) |
| `0x02F7D000` + 4 KB | IRQ stack (`header->vblank`) |
| `0x02F7E000` + 8 KB | idle and init stack (`header->halt`, `header->init`) |

It runs through the cached `0x02xxxxxx` mirror; uncached access would make rcheevos several times slower. Only DS (NTR-mode) games booted from SD use it: DSi-enhanced games in DSi mode use all 16 MB.

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

## What the player sees

- **Unlock.** The DSi's Wi-Fi LED blinks three times. It changes nothing on screen or in the sound, so it can't get in the game's way. The ARM7 reads the module's unlock count at VBlank and drives the LED over I2C, as nds-bootstrap already does for its ROM-read LED. Not on 3DS.
- **In-game menu (L+DOWN+SELECT).** It has an "Achievements..." page with the game, hardcore or softcore, unlocked/total achievements and points, the unlocks of this session, what is still waiting to be sent, and frames checked/skipped.
- **Hardcore.** The in-game menu hides the RAM viewer and RAM dump (the dump is hidden in softcore too), and the refresh-rate option, whose rates below 60 Hz act as slow motion. nds-bootstrap's front end skips user cheats. AP-fix patches, which games need to run, still apply. nds-bootstrap has no save states.

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
