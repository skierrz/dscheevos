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

1. nds-bootstrap's loader reads the DScheevos settings and login token from `_nds/dscheevos/`.
2. It hashes the ROM with rcheevos' DS hash (header, ARM9 and ARM7 binaries, icon).
3. It connects to Wi-Fi, logs in, fetches the achievement set and starts a session. Hardcore requires being online here.
4. It parses the set (rcheevos load), places the DScheevos module and its heap in the reserved RAM, and drops the session-start-only code (parsers, JSON, hashing), which can be paged back from SD if needed.
5. The game boots.

Measured cost of step 4: 0.85–1.4 s for a 60-achievement set and 14–27 s for a 250-achievement set at 67 MHz (see `docs/rcheevos-arm9-footprint.md` and `tests/do-frame-timing`).

## Every frame

```
ARM7 VBlank IRQ (nds-bootstrap CE7, existing handler)
  └─ IPC sync 0xA ──► ARM9 IPC IRQ (CE9, existing hook)
                        └─ snapshot: copy the bytes rcheevos needs into buffer N   (short, IRQ context)

ARM9 idle: game calls OS_Halt (SDK wait for interrupt)
  └─ patched to DScheevos idle entry (own stack, IRQs on)
       └─ if a snapshot is waiting: rc_client_do_frame against it
       └─ then halt as the game expected
```

- **Snapshot at VBlank.** The game's RAM is copied at a fixed point in the frame, so achievement logic always sees one consistent frame. The ARM9 does the copy (not the ARM7) because only the ARM9 sees what's in its own data cache.
- **What gets copied.** Not all 4 MB. DScheevos records the addresses rcheevos reads and copies those (rounded to cache lines). Pointer chains are followed in the same VBlank pass so the target matches the pointer of that frame.
- **Work in idle time only.** The game's own `OS_Halt` is where it waits for the next interrupt. DScheevos runs there instead of halting. Interrupts stay on, so when VBlank arrives the game takes the CPU back and DScheevos' work just pauses. It never delays the game.
- **Double buffering.** The snapshot for frame N+1 never overwrites the one being evaluated for frame N.
- **Keeping pace.** Hit counts in achievement logic count frames, so every frame has to be evaluated. If evaluation falls behind (not enough idle time), snapshots queue up; past a small limit DScheevos must stop and say so rather than skip frames silently.

## Memory

- DScheevos lives at the top of the RAM nds-bootstrap uses as ROM cache, reserved by shrinking that cache (one constant in three places in nds-bootstrap).
- It runs through the cached `0x02xxxxxx` mirror. The ROM cache uses the uncached `0x0C` mirror, and uncached access would make rcheevos several times slower.
- Budget during play: ~70 KB of code and data, 134 KB of rcheevos heap for a typical set (592 KB for a heavy one), the snapshot buffers, 4 KB of stack, plus the network stack.
- Cost: a smaller ROM cache. ROMs between roughly 10.9 and 11.9 MB that load fully into RAM today would fall back to cache mode.

## Unlocks during play

rc_client raises an unlock inside `do_frame` and hands DScheevos a request to send. DScheevos queues it; the network layer sends it from idle time, never inside an interrupt.

## Measured so far

- `rc_client_do_frame` on a typical 60-achievement set: 10.6 ms at 67 MHz (63% of a frame), 7.7 ms at 133 MHz (46%), with the cache timing model. A 250-achievement set needs 2–3 frames per frame.
- So the CPU budget is the main risk. Many games may not leave that much idle time.

## Open questions

1. **Wi-Fi during play.** Sending unlocks live needs a Wi-Fi driver, and Wi-Fi drivers run on the ARM7, which belongs to the game. Options: run a small driver from nds-bootstrap's ARM7 engine; or keep Wi-Fi only at session start and send unlocks when the session ends (breaks live unlocks); or something else. Games that use local wireless themselves need a separate answer.
2. **WPA2.** The DS-compatible Wi-Fi does open and WEP networks only. WPA2 needs the DSi's own Wi-Fi chip (the BlocksDS DSWiFi library supports it).
3. **Not enough idle time.** What to do on games that don't leave room: reduce cost (faster read path, cheaper memory layout), lower the clock budget per frame by spreading work, or mark the game unsupported.
4. **Games that never call `OS_Halt`.** Some games busy-wait; they give no idle time at all. Need to find how common this is.
5. **Pop-ups.** Showing an unlock means drawing over a game that owns both screens, all sprites and VRAM.
6. **DSi-enhanced games in DSi mode** use all 16 MB, leaving no free RAM.
7. **Homebrew games** (like the homebrew titles with RetroAchievements sets) go through nds-bootstrap's separate homebrew path, not the retail card engine, and wait for VBlank through the BIOS (`swi 0x05`) instead of `OS_Halt`. They need their own hook points.
