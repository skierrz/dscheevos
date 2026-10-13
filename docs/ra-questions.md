# Questions for RetroAchievements (RAdmin)

The questions DScheevos needs RetroAchievements to answer before it can apply for hardcore validation. They are written to be sent as they are. Each one gives the context RA needs, asks one thing, and says what DScheevos will do with each answer. Record the answers in `docs/compliance.md` ("RA answers") and update this page's status column.

Drafted 2026-10-11 (questions 1-13, first draft in `docs/overlay-research.md`), expanded 2026-10-12 with the owner's pending decisions and the compliance audit. Not sent yet.

**Before sending**, check that these statements are true of the build (fixes assigned 2026-10-12, see `docs/compliance.md`):
- Q10: the hardcore fixes from the 2026-10-12 audit have landed (`docs/compliance.md`, "Audit 2026-10-12": user patch files, widescreen code file, soft-reset boot file, patch offset cache, donor ARM7, session replay), and the 133 MHz / VRAM boost options are forced off in DScheevos sessions.
- Q2: the UI says "Casual" (RA's current word), not "Softcore".
- The owner has ruled on the decisions behind Q8, Q11 and Q14 (or wants RA's view first: then send as is).

## What DScheevos is (send this first)

DScheevos runs RetroAchievements for Nintendo DS games on a real DSi (and a 3DS in DS mode), with the game running natively on the console: no emulator. It is a GPL-3.0 fork-plus-module of nds-bootstrap (the loader TWiLight Menu++ uses to boot DS games from the SD card).

- **Evaluation is rcheevos' `rc_client`, unmodified**, built for the DSi's ARM9 (67 MHz). It runs in the game's idle time: when the game's idle thread waits for the next frame, DScheevos copies the bytes the set watches, evaluates queued frames in order, and does its networking. It never writes the game's memory and never pauses the game.
- **Before the game boots** a launcher logs in, hashes the ROM with rcheevos' own DS hash, fetches the set and starts the session over the console's Wi-Fi (WPA2). Hardcore needs to be online at that point.
- **During play** unlocks, leaderboard entries and rich presence go out live over the same Wi-Fi connection. If the connection is lost, they are kept and sent at the next start.
- **User agent:** `DScheevos/<version> (Nintendo DSi) rcheevos/<version>`.
- **Hardcore lockdown:** cheats, the RAM viewer/editor, the RAM dump and slow-motion refresh rates are blocked; nds-bootstrap has no save states, rewind or frame advance. Hardcore is the default, and the mode is fixed before the game boots.
- Source: <https://github.com/skierrz/dscheevos>. Not publicly released yet; the six-month clock hasn't started.

The one hard rule we hold ourselves to: **the game must not lose any time or behave any differently** because of DScheevos (no added lag frames, no glitches, the game's own wireless works as stock). Several questions below come from that rule: where an emulator can pause the console for free, we can't.

## Summary

| # | Topic | Ask | Status |
|---|---|---|---|
| 1 | Notices during play | Is an LED cue plus an in-game menu enough for "Measured and Trigger displayed during gameplay"? If an on-screen strip is needed, may the player place it or turn it off? | not sent |
| 2 | Hardcore indicator | Is a card before the game's first frame enough for Section E? | not sent |
| 3 | Leaderboard trackers | Required during play, or covered by "disabling leaderboard popups is okay"? | not sent |
| 4 | Delayed indicators | Is a cue that trails its frame by up to ~17 s acceptable? | not sent |
| 5 | Companion display | Does a phone/PC page on the same network count? | not sent |
| 6 | Dropped frames | Drop to casual, or report? | not sent |
| 7 | Lag frames | Evaluate a frame the game didn't finish with the last finished frame's memory? | not sent |
| 8 | Games we can't keep up with | May the client refuse a set it can't evaluate in full? | not sent |
| 9 | Games with no idle hook | May the client refuse a game it can't hook, without a per-game hack? | not sent |
| 10 | nds-bootstrap options | Anything else to block? | not sent |
| 11 | Float-operand build | Is a build flag that stores rcheevos' float constants as `float` acceptable, given proven-identical results? | not sent |
| 12 | No save states | Does a client without save states, rewind or fast-forward simplify review? | not sent |
| 13 | Live sending + offline queue | Acceptable? Max age? Do offline hardcore unlocks count? What does "securely cached" require? | not sent |
| 14 | Promoted-only request | May the client add `f=3` to `achievementsets`? | not sent |
| 15 | Game identification | Confirm DS-on-DSi dumps should hash as RetroArch does. | not sent |
| 16 | Client identity | What user agent and client name? Separate ones for DSi and 3DS? Registration step? | not sent |
| 17 | Hardware clients | Review process, six-month clock, early access for testers? | not sent |
| 18 | 3DS | Can DSi and 3DS be approved separately? | not sent |

---

## A. Notices and display (Sections A and E)

### 1. Notices during play

On a DSi, nothing can draw over a running DS game without using the game's own video hardware. We looked at every way (`docs/overlay-research.md`, `docs/display-library.md`): a pop-up that pauses the game costs 70-136 lag frames each, borrowing the game's sprites or layers corrupts the picture in some games (another DSi project logged it), and even the cheapest on-screen strip changes one display register the game can read for a few lines per frame. Our rule is zero added lag frames, so a notice must never cost the game a frame.

What DScheevos does today:
- **During play:** the DSi's Wi-Fi LED blinks 3 times per unlock, twice per leaderboard start/fail/submit, once per measured-progress update, and stays changed while a challenge indicator is showing.
- **In-game menu** (the player opens it; the game is paused): the full text of active challenges, measured progress, leaderboard trackers, this session's unlocks and the whole achievement list.

What we are designing: a small strip at the top or bottom of either screen (the player picks screen, edge and side), shown only while a notice is up, and only if it can be proven to add zero lag frames on real hardware.

**Asks:**
1. Is the LED cue plus the menu enough for "Measured and Trigger flags ... displayed during gameplay when appropriate conditions are met"?
2. If an on-screen indicator is required: may the player choose where it goes, and may they turn it off (RetroArch lets players turn off challenge indicators and progress trackers)? If they turn it off, does the LED cue still satisfy the rule?
3. If no notice can be shown in a particular game without costing it frames, is it acceptable to fall back to the LED and the menu for that game?

**What we do with the answer:** if (1) is yes, DSi hardcore ships with LED and menu, and the strip stays optional. If (1) is no and the strip can't be made free in a game, that game runs casual only on DSi until it can.

### 2. Showing hardcore mode

Before the game's first frame, a card on the top screen shows the title, HARDCORE or Casual, and the player's unlocked/total achievements and points, for 1.5 s (A, B or START skip it). The launcher and the in-game menu show the mode at any time. RA's page says the indicator is "normally done when the player starts a game".

**Ask:** is that enough for Section E, or must the mode stay on screen during play?

### 3. Leaderboard trackers

Leaderboards are evaluated, submitted and queued offline like unlocks. Trackers and results show in the in-game menu; the LED blinks on start, fail and submit. Nothing is drawn during play.

**Ask:** are live trackers part of "leaderboards must function", or covered by "disabling leaderboard popups is okay"? If required, is the menu value plus the LED cue enough?

### 4. Delayed indicators

Achievements are checked in the game's spare time. Every frame's watched bytes are queued (up to 1024 frames, about 17 s) and evaluated in order, each as its own frame, so a notice can trail the frame it describes by up to about 17 s in a busy scene. Unlocks carry the delay too; the server gets them when they are evaluated.

**Ask:** is a delayed indicator (and unlock) acceptable, given that every frame is still evaluated in order?

### 5. Companion display

The module could send each rc_client event to a page on a phone or PC on the same network. It costs the game nothing.

**Ask:** would that count as displaying notices during play, alone or together with the LED?

---

## B. Hardcore integrity

### 6. Dropped frames

If the game leaves no spare time for longer than the queue holds (about 17 s of frames whose watched bytes change), the oldest frames can't be kept, and a frame goes unchecked. Hit counts could then differ from what RetroArch would count. Frames whose watched bytes didn't change take no queue space, so cutscenes and menus rarely drop. The in-game menu shows frames checked and skipped. In our tests, sets we would ship drop none; the launcher will refuse sets that can't keep up (question 8).

**Ask:** when a frame is dropped in a hardcore session, should the session drop to casual, should the dropped frame's achievements be held back (for example, reset their hit counts), or is reporting it in the menu enough?

### 7. Lag frames

DScheevos copies the watched memory when the game finishes its frame (its idle thread halts), not in the middle of the game's work. If the game doesn't finish a frame before the next VBlank (a lag frame), there is no new copy, so that VBlank is evaluated with the last finished frame's memory, as a repeat: hit counts still advance once per VBlank. An emulator evaluating at the VBlank would see the game's half-updated memory instead.

**Ask:** is evaluating a lag frame as a repeat of the last finished frame acceptable? (The alternative, copying mid-frame inside the game's interrupt, costs the game 10-35% of a frame, which we won't ship.)

### 8. Sets too heavy for the console

A few sets need more CPU than a 67 MHz DSi has in a frame, even using all the game's spare time. Estimates from instruction counts on the real sets: The World Ends with You needs about 6-9 times a whole frame, Pokémon HeartGold/SoulSilver about 3-4 times, Digimon World DS about 1.3-2 times. Evaluating those would fall behind for good and drop frames. Most sets need 10-50% of a frame.

We will never evaluate part of a set, skip conditions or check some achievements less often (another DS project does that; it changes how achievements behave).

**Ask:** may the launcher refuse such a set up front ("too heavy for the DSi: play this game without achievements, or on an emulator")? Would you rather it allowed casual with a warning?

### 9. Games we can't hook

DScheevos gets its spare time from the NitroSDK idle thread's `OS_Halt`. Some games never use it: in our survey, Sonic Rush and Resident Evil: Deadly Silence wait only through the BIOS. Today DScheevos turns itself off for such a game and says so; we are working on a general way to cover them, with no per-game patches.

**Ask:** is it acceptable for the client to support only the games it can hook, and tell the player clearly when a game isn't supported? Does the approval cover the client as a whole, or do you want a list of supported games?

### 10. nds-bootstrap options

In a hardcore session DScheevos blocks: cheats (TWiLight's cheat list and nds-bootstrap's widescreen code file), the in-game RAM viewer (it can edit memory), the RAM dump, refresh rates below 60 Hz (slow motion), user-supplied patch files for the game, and booting anything but the hashed game. The console always runs at the DS's own speed: nds-bootstrap's options to run the DSi's ARM9 at 133 MHz or speed up VRAM are forced off in DScheevos sessions, since they shorten a game's own slowdown. It keeps nds-bootstrap's built-in compatibility patches (anti-piracy fixes, save fixes) that games need to run, button remapping (no turbo) and screen swap. nds-bootstrap has no save states, rewind, fast-forward, frame advance or input playback.

**Ask:** anything else to block in hardcore?

### 11. Float-operand build

rcheevos stores each floating-point constant of a condition as a `double`, but only ever uses it as `(float)` when evaluating (`rc_operand.c`). Memory is tight on the DSi (about 1.2 MB for everything), so we would like to build rcheevos with a forced-include header that makes that one stored field a `float`. No rcheevos source line changes; every other `double` (the parser, the MBF32/double decoders) stays a `double`. It cuts rcheevos' memory for a set by about 30% (Pokémon Platinum 559 KB to 393 KB, HG/SS 1.60 to 1.10 MB). The stored value is rounded once, exactly as the later cast rounds it, so evaluation is bit-identical; we checked the parsed result and 300 frames of evaluation against the stock build on all 32 real DS sets we have, and keep that check in CI. The only theoretical difference is a Measured target above 2^24 written as a float constant; none exists.

**Ask:** is that build acceptable for a hardcore client? If not, would `#pragma pack(4)` on the same structs (about 16%, also identical results) be?

### 12. No save states, rewind or fast-forward

None of these exist on this path: the game runs on the console as it would from a cartridge, and every start goes through the launcher and a new session.

**Ask:** does that simplify review, and which requirements do you consider not applicable?

---

## C. Sending and data

### 13. Live sending plus an offline queue

Unlocks and leaderboard entries go out during play over the console's own Wi-Fi. Every entry of the session is also kept in a journal in RAM. When the game is stopped (in-game menu open, Reset, Exit, soft reset; never during play, since SD writes during play can freeze games) the journal is written to the SD card. At the next start, the launcher sends what is unsent with the real time since each unlock (`o=`, rcheevos' `seconds_since_unlock`). Turning the console off between those points loses unsent entries since the last one. Hardcore entries are sent as hardcore.

The journal on the SD card is a plain file, as is the session file with the set. Someone with the SD card could edit either, but they could equally use their login token from the same card directly, so a checksum on the device would add no real protection.

**Asks:**
1. Is that acceptable?
2. Is there a maximum age you'll accept for an unlock sent later?
3. Should hardcore unlocks earned while offline count as hardcore?
4. What does "securely cached" require of a client like this?

### 14. Promoted-only request (`f=3`)

The set must fit in 256 KB of console RAM. Pokémon Platinum's `achievementsets` reply is 311 KB; with `f=3` (promoted achievements only) it is 195 KB, with the same 102 promoted achievements, leaderboards and rich presence. rc_client drops unpromoted achievements anyway (unofficial is off), so evaluation is identical. rcheevos' request builder has no option for it, so our code would append `&f=3` to the request rcheevos built. We already ask for gzip.

**Ask:** is adding `f=3` acceptable?

### 15. Game identification

The launcher hashes the ROM with rcheevos' own DS hash (`rc_hash`, unmodified), so a dump hashes as in RetroArch. 35 of our 64 dumps match a set; some (NSMB, Animal Crossing, Clubhouse Games) show "Unsupported Game Version".

**Ask:** please confirm that DS games run on a DSi should hash as in RetroArch (same dumps, same hashes), and that we should not hash anything else.

### 16. Client identity

Today: `DScheevos/<version> (Nintendo DSi) rcheevos/<version>` from both the launcher and the in-game module. On a 3DS in DS mode the same build runs today and also says "Nintendo DSi".

**Asks:**
1. Is that the user agent and client name you want?
2. If DSi and 3DS are approved separately (question 18), should a 3DS send `(Nintendo 3DS)`?
3. Is there a registration step for a new client?

---

## D. Approval

### 17. Hardware clients

Your FAQ says manual unlocks aren't granted for original hardware.

**Asks:**
1. What review process do you want for a client that isn't an emulator?
2. Does the six-month clock apply the same way (counted from the first public release)?
3. Can testers get early access, for example hardcore unlocks counted for a few named test accounts before validation?

### 18. 3DS

On a 3DS the DS picture passes through the system's scaler (TwlBg), which can draw a full overlay without touching the game. So the 3DS can show notices like RetroArch does.

**Ask:** can DSi and 3DS be approved separately, given their different notice capabilities?

---

## Earlier numbering

| Here | 2026-10-11 draft (chat) | `docs/overlay-research.md` |
|---|---|---|
| 1-5 | 1-5 | 1-4 (5 was the LAN display) |
| 6 | 6 | - |
| 10 | 7 | - |
| 12 | 8 | 6 |
| 13 | 9 | 7 |
| 15 | 10 | - |
| 16 | 11 | - |
| 17 | 12 | 6 |
| 18 | 13 | 8 |
| 7, 8, 9, 11, 14 | new (owner decisions pending, 2026-10-12) | - |
