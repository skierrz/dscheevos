# RetroAchievements hardcore compliance

DScheevos has to meet every point of RetroAchievements' [Hardcore Compliance Requirements](https://docs.retroachievements.org/general/hardcore-compliance-requirements.html) before RA will count its hardcore unlocks. This page goes through the list point by point: what DScheevos does today, where in the code, and what is still missing.

Until RA approves the client, the server files every unlock as softcore, even with `h=1`: "If a User-Agent header is not provided, not recognized," the server will "turn hardcore unlock requests into softcore unlock requests", and clients must "contact RAdmin" to be "validated for hardcore support" ([rcheevos rc_client integration guide](https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration)). Another nds-bootstrap fork measured this on a real account ([nds-bootstrap-ra v0.1.0 notes](https://github.com/Bakkerrrs/retroachievements-ds-on-3ds/blob/main/RELEASE-v0.1.0.md)). No code change gets around it; approval does.

**How DScheevos shows this.** The server marks an unrecognised client by adding a warning achievement (ID 101000001 and up, "Warning: Unsupported Emulator") to the set. The launcher and the module look for it when the set loads. Hardcore then shows as "Rules on, RA counts as Casual" in the launcher and the in-game menu; without the warning it shows "Not yet confirmed by RA", since we haven't yet seen the server count one of our hardcore unlocks. The hardcore lockouts stay on either way. The warning is never shown or counted as an unlock. The mock server (`tools/mock-ra-server`) adds the warning and files hardcore unlocks as softcore unless run with `--known-client`.

**Testing against the real server** (needs a test account):
- rc_client's spectator mode (`rc_client_set_spectator_mode_enabled`) evaluates and shows unlocks but sends none: use it for bench runs on a real account.
- To check how the server files an unlock, earn one for real, then read the account's game page; reset the game's progress there afterwards (RA's FAQ: hover the percentage on the game page, or use the settings page; achievements can also be reset one by one).

Status: **done**, **partial**, **missing**, or **n/a** (the feature doesn't exist on DS hardware or in nds-bootstrap).

## A. RetroAchievements features

| Requirement | Status | Where / what's left |
|---|---|---|
| Achievement list reachable in the client | **done** | In-game menu > "Achievements..." (`nds-bootstrap: arm9_igm/dscheevosMenu.c`): a summary page (game, mode, totals, this session's unlocks, network), then every achievement of the set with its state, points and measured progress, and a detail page with the description. The module publishes the list (`dscm_list_t`, ABI v6). |
| Triggers evaluate correctly | **done** | rcheevos `rc_client`, unmodified. `tests/games` checks unlock frames against rcheevos reading live memory every frame, with and without a 10 s backlog. |
| Measured and Trigger (challenge) indicators shown in the list and during play | **partial** | In the list: measured progress and an active-challenge mark per achievement, plus the active challenges and leaderboard trackers on the summary page. During play (DSi): Wi-Fi LED cues from the module's notices: 3 blinks per unlock, 1 per measured-progress update, 2 per leaderboard start/fail/submit, held while a challenge is primed. Nothing is drawn over the game; whether RA accepts this is question 1 in `docs/ra-questions.md`. |
| Rich presence works | **done** | rc_client pings every 2 minutes during play. A hardcore session always keeps the Wi-Fi connection for it; Casual does when "Send unlocks during play" is on. |
| Leaderboards work | **partial** | Evaluated, submitted and queued offline like unlocks; trackers and results show in the in-game menu. No on-screen tracker or start/fail notice during play (see the indicators row). |
| Offline queueing: unlocks made offline are cached and sent later | **done** | The module keeps every unlock and leaderboard entry of the session in a journal (`dscm_journal_t`); nds-bootstrap's ARM7 copies it to `_nds/dscheevos/pending.bin` at safe points only (in-game menu opening, Reset, Exit, the soft-reset and return combos, a reset the game asks for). The launcher sends what is unsent at the next start with the time since each unlock (`o=`), and sets a file aside in `unsent/` if it still can't. Turning the console off without one of those safe points loses what was unsent since the last one. Tested in melonDS (`tests/launcher/run.sh` with `--fail-awards`, then a normal run). RA asks that offline unlocks be "securely cached" without saying what that means: `pending.bin` (and the set in `session.bin`) are plain files, as editable as the login token next to them, so a checksum on the console would add nothing real. RA question 13. |
| Save state hit storage | n/a | nds-bootstrap has no save states. |
| RAIntegration (Windows) | n/a | No Windows build. |
| Standard save files | **done** | nds-bootstrap's `.sav` files, the same raw format emulators use. |

## B. Hardcore rules

| Requirement | Status | Where |
|---|---|---|
| Cheats off | **done** | Hardcore session: `cheatSize = 0` (`nds-bootstrap: arm9/source/dscheevos_session.c`), so TWiLight's cheat list never loads; `wideCheatSize = 0` too (`wideCheatData.bin` runs as cheat codes). The other SD-card paths that could change the game are closed too: see the audit below and `tests/hardcore/lockdown.sh`. |
| No rewind | n/a | Not in nds-bootstrap. |
| No slowdown or frame advance | **done** | The in-game menu's refresh-rate option (slow motion) is hidden in hardcore (`arm9_igm/inGameMenu.c`). No frame advance exists. The CPU and VRAM speed options (in-game menu, `BOOST_CPU`/`BOOST_VRAM`, `DSI_MODE = 2` on a DS game) make a game's own slowdown shorter, not longer: the player's choice, including in hardcore (owner ruling B, provisional, 2026-10-12): off by default, chosen before launch, locked for the session (the in-game menu can't switch it), nds-bootstrap's `twlClockExcludeList` honoured (also for `DSI_MODE = 2`, which counts as the CPU boost), recorded in the module status and journal; DScheevos never turns it on, and its CPU gate assumes 67 MHz. RA's text bans slowdown, not speed-ups; this choice is ours until RA can be asked. |
| Loading save states blocked | n/a | Not in nds-bootstrap. |
| Rich presence and leaderboards can't be turned off | **done** | No setting turns them off, and in hardcore the session always keeps Wi-Fi ("Send unlocks during play" is forced on and shown so). |
| Resume must drop to casual | n/a | No resume feature. Every start goes through the launcher and a new session. |
| No switch from casual to hardcore mid-session | **done** | The mode is chosen in the launcher before the game boots. The session file is used once. Nothing in the game changes it. |
| Memory editors, debuggers, scripting/TAS/input playback | **done** | RAM viewer (it can edit memory) hidden in hardcore; RAM dump hidden whenever DScheevos runs. No scripting or input playback exists in shipped builds (the lab rig's input injection is a lab-only build, never shipped). |

## C. Identity

| Requirement | Status | Where |
|---|---|---|
| Unique, stable user agent `Name/version (OS) core/version` | **done** | `DScheevos/<VERSION> (Nintendo DSi) rcheevos/<version>`, from the launcher (`launcher/source/session.c`) and the module (`module/src/module.c`). `VERSION` at the repo root is the only place the number lives; the release workflow refuses a tag that doesn't match it. The rcheevos part comes from rcheevos itself (`rc_client_get_user_agent_clause` in the launcher, `RCHEEVOS_VERSION_STRING` in the module). A 3DS also says "Nintendo DSi" today; RA question 16. |
| Never identified as another client | **done** | DScheevos has always sent its own user agent. |

## D. Timeline

| Requirement | Status |
|---|---|
| Publicly available for 6 months before applying | **not started**: the clock starts at the first public release. |

## E. Defaults and UX

| Requirement | Status | Where |
|---|---|---|
| Hardcore on by default (recommended) | **done** | `launcher/source/config.c` defaults to `HARDCORE = 1`. |
| Hardcore state shown during play | **partial** | Before the game's first frame, a card on the top screen shows the title, HARDCORE or Casual and the totals for 1.5 s (A, B or START skip it); the launcher and the in-game menu show the mode too. Nothing on screen while the game runs; RA question 2 in `docs/ra-questions.md`. RA now calls the other mode "Casual"; our UI still says "Softcore" (renaming assigned 2026-10-12). |

## F. Transparency and legality

| Requirement | Status | Where / what's left |
|---|---|---|
| Monetization disclosure | **done** | None: DScheevos is free, with no paid features, ads or required donations. Stated in `README.md` and the zip's `README.txt`. |
| Licences of everything shipped | **done** | The zip's `_nds/dscheevos/licenses/` has DScheevos (GPL-3.0), rcheevos, mbedTLS, lwIP, nds-bootloader, nds-bootstrap, and the toolkit libraries linked into the binaries: BlocksDS libnds, DSWiFi, start-up code and picolibc (launcher); devkitARM newlib (module), libnds 1.8 and libfat (nds-bootstrap), libnds 2 and calico (`load.bin`). The toolkit texts are vendored in `release/licenses/`; `SOURCES.txt` says which binary links what. |
| GPL obligations for shipped binaries | **done** | DScheevos, nds-bootstrap (GPL-3.0) and nds-bootloader (GPL-2.0+): `SOURCES.txt` names the exact commits, and the zip carries each one's licence text. |
| Privacy policy | **done** | `docs/privacy.md`, shipped as `privacy.txt`: what goes to RetroAchievements (username, token, game hash, unlocks, leaderboard entries, rich presence), the Wi-Fi key kept in RAM only, every file on the SD card including the offline queue, how long each is kept and where (RA's page now asks for retention periods, server locations and GDPR handling: added 2026-10-12), how to delete it, contact through the issue tracker. |

## G. Auto-fail criteria

| Criterion | Status |
|---|---|
| Loading save states in hardcore | not possible (B) |
| Rewind, slow motion, frame advance in hardcore | blocked (B) |
| Gameplay-altering cheats in hardcore | blocked (B) |
| Switching to hardcore without a reset | not possible (B) |
| Non-unique user agent | own user agent (C) |
| Undisclosed use of another client's user agent | never happened (C) |
| Non-commercial cores with commercialization | none shipped, no commercialization |
| Privacy policy with placeholders or contradictions | complete policy, no placeholders (F) |

## Real-server results

Bench runs on the test account, per `tests/real/README.md` (melonDS, Tetris DS (USA, Australia), `tetris-marathon.txt`, `FRAMES=4500`).

| Date | Build | Run | Result |
|---|---|---|---|
| 2026-10-12 | perf | 1. `--check` | **pass**: `login2` answered HTTP 401 over TLS (also passed in ra-compliance's own run before the merge) |
| 2026-10-12 | perf `390202f` | 2. spectator | **pass**: `login2`, `achievementsets` HTTP 200, no `startsession`; 230051 unlocked locally at game frame 972, 0 sent; hardcore state 2 (the server's warning entry seen); 0 frames dropped; nothing new on the website |
| 2026-10-12 | perf `390202f` | 3. live | **pass**: `login2`, `achievementsets`, `startsession` HTTP 200; 230051 unlocked at game frame 852 and sent (1 sent); the website shows exactly that one unlock, **as casual**, as expected for an unrecognised client, and Tetris DS as last played; 0 frames dropped; no token in any log |
| | | 4. clean-up | owner's step; not reported |
| 2026-10-12 | perf `c08df65` | 6. rich presence (`FRAMES=12000`) | **pass**: `login2`, `achievementsets`, `startsession` HTTP 200; pings carried the set's own line and the profile showed "Standard \| Marathon \| Score: 60 \| Lines Cleared: 0" for Tetris DS; 9,544 frames evaluated, 0 dropped. 230051 unlocked again locally (game frame 1272) and the server answered "User already has this achievement unlocked": no second unlock, no hardcore credit (progress hadn't been reset after step 3) |

Not covered yet: offline-queue timing (see "Open" below).

## Open

- **Offline unlock times off by the console's UTC offset.** The module reads the DSi's clock, which holds local time, as if it were UTC, so a queued unlock sent later carries an award time a few hours off (about 4 h for the owner's console). Live unlocks are unaffected (the server stamps them). Product bug, fix in progress elsewhere; re-test with the offline-queue check in `docs/handoff.md` once it lands.

## Strict reading (owner, 2026-10-12)

Meeting RA's published list is imperative, and the RA questions are on hold until DScheevos is ready to show. So nothing may rely on RA accepting an interpretation: each requirement is met on its plain, strictest reading. Zero gameplay cost still holds; where the two collide, the owner rules.

| Requirement (RA's words) | Strict reading | Today | Gap and plan |
|---|---|---|---|
| A: Measured and Trigger flags "displayed during gameplay when appropriate conditions are met" | on screen, during play | LED cues + menu | **Missing.** An on-screen indicator is required, not optional. It must add zero lag frames (owner ruling). Display session (`docs/display-library.md`): the top/bottom strip with a placement setting. Its residuals (a display register the game can read for a few lines, main-bus time while shown) need the owner's ruling, since "nothing on screen is strictly zero-cost on a DSi". |
| A: offline unlocks "securely cached and sync ... when connectivity returns" | no unlock is ever lost, and the cache can't be casually edited | journal saved at safe points only; power-off between them loses unsent unlocks; `pending.bin` is a plain file | **Partial.** (1) Close the power-off gap: save the journal when the game itself writes its save (nds-bootstrap already writes to SD then: owner decision pending), on lid close/sleep, and check for a power-button grace period. (2) Tamper-evidence: the launcher refuses a journal that fails a MAC tied to the session (stops hand-edited files; the token beside it limits what any on-device secret can do). |
| A: triggers "work properly" | every frame evaluated as an emulator would | queue of 1024 frames; a dropped frame is counted and shown | **Partial.** A dropped frame means triggers may not work properly. Hardcore session drops to casual on the first dropped frame (hardcore to casual mid-session is allowed by B), and the launcher's CPU gate refuses sets that can't keep up, so a dropped frame should never happen in practice. |
| A: leaderboards "must function" | evaluated and submitted | yes, queued offline too | done (trackers aren't required by the text) |
| E: hardcore state "visibly indicated", "normally ... a brief informational message" when the game starts | a message at game start | pre-boot card, 1.5 s | done; wording "Casual" (fix assigned (developer-db)) |
| B: cheats, slow motion, memory editors | blocked in hardcore | see audit below | fixes assigned (developer-db) |
| F: privacy policy: retention, server locations, GDPR | all present | added 2026-10-12 | done |
| C: unique user agent | yes | yes | done; pin an rcheevos release tag so the version is exact |
| D: public for six months | public release | not released | starts at the first public release |

## Audit 2026-10-12

Against RA's page as of 2026-10-12 (it now says "Casual" for softcore, and asks the privacy policy for retention periods, server locations and GDPR handling). Product fixes are assigned by developer-db; this table tracks them.

**Hardcore: SD card and ini paths that change the game** (paths in nds-bootstrap `retail/`):

| Path | What it can do | Today | Fix |
|---|---|---|---|
| `_nds/nds-bootstrap/apFix/*.ips` / `*.bin` (`arm9/source/pck_load.cpp` `loadApFix`) | patch any code (IPS), or feed Action Replay codes to the cheat engine (`.bin`); found before the built-in `nitro:/apfix.pck` | blocked in hardcore | fixed: hardcore uses only the built-in pack (`loadApFix`); a game with a user file and no built-in fix isn't offered hardcore by the launcher (Casual or no achievements) |
| `_nds/nds-bootstrap/wideCheatData.bin` (`arm9/source/conf_sd.cpp`, `bootloaderi/source/arm7/hook_arm7.c`) | any Action Replay codes, not only widescreen | blocked in hardcore | fixed: `wideCheatSize = 0` in hardcore |
| `_nds/nds-bootstrap/softResetParams.bin` + `pagefile.sys` (`bootloaderi/source/arm7/main.arm7.c`) | `LOAD` boots code from the page file; `srlAddr` boots another program inside the ROM file, outside the DS hash | blocked in hardcore | fixed: a hardcore boot clears `LOAD` and the SRL offset before they are read (`dscheevosSanitizeSoftReset`); every DScheevos boot comes through the launcher, so no session loses a legitimate hand-over; a game that resets into a Download Play child restarts from its own binaries |
| `patchOffsetCache/<TID>-<CRC>.bin` | nds-bootstrap's patches written at addresses from the file, unchecked | ignored in hardcore | fixed: the bootloader finds the offsets by signature in hardcore ("Please wait" shows each time) |
| `DONOR20_NDS_PATH` (one title) | replaces the game's ARM7 binary | blocked in hardcore | fixed: no donor in hardcore; the launcher doesn't offer hardcore for that build while a donor is set |
| `session.used` renamed back, another ROM at the same path | a session hashed for one ROM runs another (nds-bootstrap matches the path only) | blocked | fixed: the front end refuses a session written over 10 minutes earlier (console clock), and the bootloader zeroes `session.used` once loaded |
| in-game menu clock speed and VRAM mode; `BOOST_CPU`, `BOOST_VRAM`; `DSI_MODE = 2` on a DS game | 133 MHz / faster VRAM: shorter in-game slowdown | allowed, locked for the session | the player's choice, including in hardcore (owner ruling B, provisional, 2026-10-12): off by default, chosen before launch, locked for the session (the in-game menu can't switch it), nds-bootstrap's `twlClockExcludeList` honoured; DScheevos never turns it on, and its CPU gate assumes 67 MHz; done: the in-game menu hides CLOCK_SPEED and VRAM_MODE in DScheevos sessions, `DSI_MODE = 2` on a DS game runs in DS mode as the CPU boost (before, it switched achievements off), excluded games stay at 67 MHz, and the choice is recorded in the module status and the journal (`clock`) |
| cheat list (`cheatData.bin`), RAM viewer, RAM dump, refresh rate | cheats, memory editing, slow motion | blocked | done |
| key remaps, screen swap, region/language, colour LUT, sound rate, sleep, manual, save path | controls, display, compatibility; saves are user data | allowed | none needed |

**Other findings:**
- **Offline queue "securely cached".** `pending.bin` and the set in `session.bin` are plain files beside the login token; a checksum on the console would protect nothing the token doesn't already expose. RA question 13 is on hold: see "Strict reading".
- **User agent on 3DS** says "Nintendo DSi". Send "(Nintendo 3DS)" there, so the two can be told apart without asking (RA question 16, on hold).
- **rcheevos version.** The submodule is upstream `develop` at `683197d` (v12.5.0 plus 9 commits, unmodified), which reports itself as 12.5.0. Pin a release tag before applying, so the user agent's version names exactly the code shipped.
- **Wording.** "Softcore" is "Casual" in the launcher, placard and in-game menu (done).
- **Spectator mode** sends no unlocks, leaderboard entries or session start; its journal is discarded (`pending.spectator`). Matches rc_client's spectator mode; nothing to change.
- **What is sent** matches `docs/privacy.md`: login, game hash, set, session start, unlocks, leaderboard entries, rich presence pings, all to `retroachievements.org` with the DScheevos user agent. The real-account bench run (`tests/real/`) checks this against the live server.

## Targets

1. **DSi / DSi XL** (DS games in DS mode): the first target.
2. **3DS**: second. There, the DS picture passes through TwlBg (the 3DS program that scales it), which can be patched (TWPatch does). Drawing the overlay there touches nothing the game uses and has room for badge images, so the 3DS version can look closer to RetroArch's.
3. **DS via flashcard** (B4DS mode): later, and a separate design rather than a setting: 4 MB RAM and no DSi extra RAM for the module, no WPA2 Wi-Fi, and nds-bootstrap's DScheevos hook refuses flashcards today (`arm9/source/dscheevos_session.c`).

## What's left, in order

1. On-screen notices during play. Built so far without touching the game: the pre-boot card with the mode and the Wi-Fi LED cues. Anything drawn over the game needs the owner's decision and RetroAchievements' answer first (`docs/overlay-research.md`): on the DSi nothing can draw over the game without touching its display state.
2. Ask RetroAchievements (RAdmin) to validate the client; the questions are in `docs/ra-questions.md`.
3. Real hardware: the tests listed in `docs/handoff.md` and `docs/overlay-research.md`.
4. A first public release, which starts the 6-month clock.
