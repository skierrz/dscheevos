# RetroAchievements hardcore compliance

DScheevos has to meet every point of RetroAchievements' [Hardcore Compliance Requirements](https://docs.retroachievements.org/general/hardcore-compliance-requirements.html) before RA will count its hardcore unlocks. This page goes through the list point by point: what DScheevos does today, where in the code, and what is still missing.

Until RA approves the client, the server files every unlock as softcore, even with `h=1`: "If a User-Agent header is not provided, not recognized," the server will "turn hardcore unlock requests into softcore unlock requests", and clients must "contact RAdmin" to be "validated for hardcore support" ([rcheevos rc_client integration guide](https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration)). Another nds-bootstrap fork measured this on a real account ([nds-bootstrap-ra v0.1.0 notes](https://github.com/Bakkerrrs/retroachievements-ds-on-3ds/blob/main/RELEASE-v0.1.0.md)). No code change gets around it; approval does.

**How DScheevos shows this.** The server marks an unrecognised client by adding a warning achievement (ID 101000001 and up, "Warning: Unsupported Emulator") to the set. The launcher and the module look for it when the set loads. Hardcore then shows as "Rules on, RA counts as softcore" in the launcher and the in-game menu; without the warning it shows "Not yet confirmed by RA", since we haven't yet seen the server count one of our hardcore unlocks. The hardcore lockouts stay on either way. The warning is never shown or counted as an unlock. The mock server (`tools/mock-ra-server`) adds the warning and files hardcore unlocks as softcore unless run with `--known-client`.

**Testing against the real server** (needs a test account):
- rc_client's spectator mode (`rc_client_set_spectator_mode_enabled`) evaluates and shows unlocks but sends none: use it for bench runs on a real account.
- To check how the server files an unlock, earn one for real, then read the account's game page; reset the game's progress there afterwards (RA's FAQ: hover the percentage on the game page, or use the settings page; achievements can also be reset one by one).

Status: **done**, **partial**, **missing**, or **n/a** (the feature doesn't exist on DS hardware or in nds-bootstrap).

## A. RetroAchievements features

| Requirement | Status | Where / what's left |
|---|---|---|
| Achievement list reachable in the client | **partial** | In-game menu > "Achievements..." (`nds-bootstrap: arm9_igm/dscheevosMenu.c`) shows the game, mode, totals and this session's unlocks. It doesn't list every achievement with its description and state yet. |
| Triggers evaluate correctly | **done** | rcheevos `rc_client`, unmodified. `tests/games` checks unlock frames against rcheevos reading live memory every frame, with and without a 10 s backlog. |
| Measured and Trigger (challenge) indicators shown in the list and during play | **missing** | Nothing draws over the game. An unlock only blinks the Wi-Fi light. Needs an on-screen notice during play. |
| Rich presence works | **partial** | rc_client pings every 2 minutes, but only when "Send unlocks during play" is on: that's the only network path during play. |
| Leaderboards work | **partial** | Evaluated and submitted like unlocks (same network path). No on-screen tracker or start/fail notice yet. |
| Offline queueing: unlocks made offline are cached and sent later | **missing** | Requests wait in RAM only. Turning the console off loses them (`release/README.txt`). Needs a file on the SD card that the launcher sends at the next start. |
| Save state hit storage | n/a | nds-bootstrap has no save states. |
| RAIntegration (Windows) | n/a | No Windows build. |
| Standard save files | **done** | nds-bootstrap's `.sav` files, the same raw format emulators use. |

## B. Hardcore rules

| Requirement | Status | Where |
|---|---|---|
| Cheats off | **done** | Hardcore session: `cheatSize = 0` (`nds-bootstrap: arm9/source/dscheevos_session.c`), so TWiLight's cheat list never loads. |
| No rewind | n/a | Not in nds-bootstrap. |
| No slowdown or frame advance | **done** | The in-game menu's refresh-rate option (slow motion) is hidden in hardcore (`arm9_igm/inGameMenu.c`). No frame advance exists. |
| Loading save states blocked | n/a | Not in nds-bootstrap. |
| Rich presence and leaderboards can't be turned off | **partial** | No setting turns them off, but with "Send unlocks during play" off, rich presence never reaches the server. Hardcore must keep the network path on, or the setting must go. |
| Resume must drop to casual | n/a | No resume feature. Every start goes through the launcher and a new session. |
| No switch from casual to hardcore mid-session | **done** | The mode is chosen in the launcher before the game boots. The session file is used once. Nothing in the game changes it. |
| Memory editors, debuggers, scripting | **done** | RAM viewer (it can edit memory) hidden in hardcore; RAM dump hidden whenever DScheevos runs. |

## C. Identity

| Requirement | Status | Where |
|---|---|---|
| Unique, stable user agent `Name/version (OS) core/version` | **done** | `DScheevos/<VERSION> (Nintendo DSi) rcheevos/<version>`, from the launcher (`launcher/source/session.c`) and the module (`module/src/module.c`). `VERSION` at the repo root is the only place the number lives; the release workflow refuses a tag that doesn't match it. |
| Never identified as another client | **done** | DScheevos has always sent its own user agent. |

## D. Timeline

| Requirement | Status |
|---|---|
| Publicly available for 6 months before applying | **not started**: the clock starts at the first public release. |

## E. Defaults and UX

| Requirement | Status | Where |
|---|---|---|
| Hardcore on by default (recommended) | **done** | `launcher/source/config.c` defaults to `HARDCORE = 1`. |
| Hardcore state shown during play | **partial** | The launcher shows the mode before the game boots, and the in-game menu shows it. Nothing shows it once the game is running. |

## F. Transparency and legality

| Requirement | Status | Where / what's left |
|---|---|---|
| Monetization disclosure | **done** | None: DScheevos is free, with no paid features. Say so in the README. |
| Licences of everything shipped | **partial** | The zip has `_nds/dscheevos/licenses/` (rcheevos, mbedTLS, lwIP, nds-bootloader, nds-bootstrap) and `SOURCES.txt`. DScheevos itself is GPL-3.0 (`LICENSE`). Missing: DScheevos' own licence in the zip, and the BlocksDS and devkitARM runtime libraries linked into the launcher and module (libnds, DSWiFi, newlib). |
| GPL obligations for shipped binaries | **partial** | DScheevos, nds-bootstrap (GPL-3.0) and nds-bootloader (GPL-2.0+): `SOURCES.txt` names the exact commits. The zip should carry the GPL text for DScheevos too. |
| Privacy policy | **missing** | Must say what goes where: username, login token and game hashes to RetroAchievements only; nothing else collected; what stays on the SD card. |

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
| Privacy policy with placeholders or contradictions | policy still to write (F) |

## Targets

1. **DSi / DSi XL** (DS games in DS mode): the first target.
2. **3DS**: second. There, the DS picture passes through TwlBg (the 3DS program that scales it), which can be patched (TWPatch does). Drawing the overlay there touches nothing the game uses and has room for badge images, so the 3DS version can look closer to RetroArch's.
3. **DS via flashcard** (B4DS mode): later, and a separate design rather than a setting: 4 MB RAM and no DSi extra RAM for the module, no WPA2 Wi-Fi, and nds-bootstrap's DScheevos hook refuses flashcards today (`arm9/source/dscheevos_session.c`).

## What's left, in order

1. Offline queue: unlocks and leaderboard entries made during play written to the SD card, sent by the launcher at the next start.
2. Notices during play: unlock, measured progress, challenge indicators, leaderboard start, tracker and result, and the mode at game start.
3. Full achievement list in the in-game menu, with descriptions, state and measured progress.
4. Rich presence in hardcore: keep the network path on while a hardcore session runs.
5. Privacy policy, and the missing licences in the zip (DScheevos' own, the toolkit libraries).
6. A first public release, which starts the 6-month clock.
