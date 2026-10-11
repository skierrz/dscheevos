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
- **What gets copied.** Not all 4 MB. DScheevos records the addresses rcheevos reads and copies those runs. Pointer chains are followed in the same VBlank pass so the target matches the pointer of that frame.
- **Work in idle time only.** `OS_Halt` runs in NitroSDK's idle thread, the lowest priority. NitroSDK switches threads when an IRQ returns, so the moment a game thread wakes (VBlank, card, sound) it takes the CPU back and DScheevos' work pauses mid-frame. It never delays the game.
- **A queue, not a fixed pair of buffers.** Snapshots wait in a ring (up to 1024 frames, sharing 256 KB). Each frame takes only the bytes the set watches, typically a few hundred. Games can go seconds without idle time: Kirby Super Star Ultra gives none while it formats a new save. Evaluation catches up afterwards, in order. Only if the ring fills is a frame dropped and counted as `frames_dropped`. Hit counts would then differ from RetroArch's, so the in-game menu shows it.
- **Resets.** nds-bootstrap's reset (the in-game menu's "Reset Game", or a soft reset) calls `header->reset`. Achievement state resets in the next idle pass, as an emulator calls `rc_client_reset`. Frames queued from before the reset are discarded.

## Memory

DScheevos uses 2 MB at `0x02D80000`–`0x02F80000` (the DSi's extra RAM, cached mirror). nds-bootstrap's ROM cache for DS games normally covers it, so the cache is cut to end at `0x0CD80000` (the same RAM through the uncached mirror); ROMs that loaded fully into RAM within about 2.4 MB of the old limit now use cache mode.

| Range | Use |
|---|---|
| `0x02D80000` + 256 KB | module image: header, code, data, bss (~114 KB + 10 KB today) |
| `0x02DC0000` + 384 KB | session file: boot block and server replies |
| `0x02E20000` – `0x02F7D000` | heap: rcheevos game data, address tables, 60 snapshot slots (~430 KB at 4096 addresses) |
| `0x02F7D000` + 4 KB | IRQ stack (`header->vblank`) |
| `0x02F7E000` + 8 KB | idle and init stack (`header->halt`, `header->init`) |

It runs through the cached `0x02xxxxxx` mirror; uncached access would make rcheevos several times slower. Only DS (NTR-mode) games booted from SD use it: DSi-enhanced games in DSi mode use all 16 MB.

## Unlocks during play

rc_client raises an unlock inside `do_frame` and hands DScheevos a request to send. DScheevos queues it; the network layer sends it from idle time, never inside an interrupt. (Sending during play is still being built: see open question 1.)

## What the player sees

- **Unlock.** The DSi's Wi-Fi LED blinks three times. It changes nothing on screen or in the sound, so it can't get in the game's way. The ARM7 reads the module's unlock count at VBlank and drives the LED over I2C, as nds-bootstrap already does for its ROM-read LED. Not on 3DS.
- **In-game menu (L+DOWN+SELECT).** It has an "Achievements..." page with the game, hardcore or softcore, unlocked/total achievements and points, the unlocks of this session, what is still waiting to be sent, and frames checked/skipped.
- **Hardcore.** The in-game menu hides the RAM viewer and RAM dump, and the refresh-rate option, whose rates below 60 Hz act as slow motion. nds-bootstrap's front end skips user cheats. AP-fix patches, which games need to run, still apply. nds-bootstrap has no save states.

## Measured so far

- `rc_client_do_frame` on a typical 60-achievement set: 10.6 ms at 67 MHz (63% of a frame), 7.7 ms at 133 MHz (46%), with the cache timing model. A 250-achievement set needs 2–3 frames per frame.
- So the CPU budget is the main risk. Many games may not leave that much idle time.
- Kirby Super Star Ultra (USA) through the DScheevos nds-bootstrap in melonDS: 1526 of 1531 VBlanks snapshotted and evaluated over 1800 frames, none dropped; the 5 missed were before the game turned on its interrupts. A 300-frame test achievement unlocked at frame 306.

## Open questions

1. **Wi-Fi during play.** Sending unlocks live needs a Wi-Fi driver, and Wi-Fi drivers run on the ARM7, which belongs to the game. Options: run a small driver from nds-bootstrap's ARM7 engine; or keep Wi-Fi only at session start and send unlocks when the session ends (breaks live unlocks); or something else. Games that use local wireless themselves need a separate answer.
2. **WPA2.** The DS-compatible Wi-Fi does open and WEP networks only. WPA2 needs the DSi's own Wi-Fi chip (the BlocksDS DSWiFi library supports it).
3. **Not enough idle time.** What to do on games that don't leave room: reduce cost (faster read path, cheaper memory layout), lower the clock budget per frame by spreading work, or mark the game unsupported.
4. **Games that never call `OS_Halt`.** Some games busy-wait; they give no idle time at all. Need to find how common this is. DScheevos turns itself off when it finds no `OS_Halt` (the bootstrap log says "DScheevos off: no OS_Halt").
5. **Pop-ups.** Showing an unlock means drawing over a game that owns both screens, all sprites and VRAM.
6. **DSi-enhanced games in DSi mode** use all 16 MB, leaving no free RAM.
7. **Homebrew games** (like the homebrew titles with RetroAchievements sets) go through nds-bootstrap's separate homebrew path, not the retail card engine, and wait for VBlank through the BIOS (`swi 0x05`) instead of `OS_Halt`. They need their own hook points.
