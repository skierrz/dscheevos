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
| Achievement list reachable in the client | **done** | In-game menu > "Achievements..." (`nds-bootstrap: arm9_igm/dscheevosMenu.c`): a summary page (game, mode, totals, this session's unlocks, network), then every achievement of the set with its state, points and measured progress, and a detail page with the description. The module publishes the list (`dscm_list_t`, ABI v6). |
| Triggers evaluate correctly | **done** | rcheevos `rc_client`, unmodified. `tests/games` checks unlock frames against rcheevos reading live memory every frame, with and without a 10 s backlog. |
| Measured and Trigger (challenge) indicators shown in the list and during play | **partial** | In the list: measured progress and an active-challenge mark per achievement, plus the active challenges and leaderboard trackers on the summary page. During play (DSi): Wi-Fi LED cues from the module's notices: 3 blinks per unlock, 1 per measured-progress update, 2 per leaderboard start/fail/submit, held while a challenge is primed. Nothing is drawn over the game; whether RA accepts this is question 1 in `docs/overlay-research.md`. |
| Rich presence works | **done** | rc_client pings every 2 minutes during play. A hardcore session always keeps the Wi-Fi connection for it; softcore does when "Send unlocks during play" is on. |
| Leaderboards work | **partial** | Evaluated, submitted and queued offline like unlocks; trackers and results show in the in-game menu. No on-screen tracker or start/fail notice during play (see the indicators row). |
| Offline queueing: unlocks made offline are cached and sent later | **done** | The module keeps every unlock and leaderboard entry of the session in a journal (`dscm_journal_t`); nds-bootstrap's ARM7 copies it to `_nds/dscheevos/pending.bin` at safe points only (in-game menu opening, Reset, Exit, the soft-reset and return combos, a reset the game asks for). The launcher sends what is unsent at the next start with the time since each unlock (`o=`), and sets a file aside in `unsent/` if it still can't. Turning the console off without one of those safe points loses what was unsent since the last one. Tested in melonDS (`tests/launcher/run.sh` with `--fail-awards`, then a normal run). |
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
| Rich presence and leaderboards can't be turned off | **done** | No setting turns them off, and in hardcore the session always keeps Wi-Fi ("Send unlocks during play" is forced on and shown so). |
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
| Hardcore state shown during play | **partial** | Before the game's first frame, a card on the top screen shows the title, HARDCORE or Softcore and the totals for 1.5 s (A, B or START skip it); the launcher and the in-game menu show the mode too. Nothing on screen while the game runs; RA question 2 in `docs/overlay-research.md`. |

## F. Transparency and legality

| Requirement | Status | Where / what's left |
|---|---|---|
| Monetization disclosure | **done** | None: DScheevos is free, with no paid features, ads or required donations. Stated in `README.md` and the zip's `README.txt`. |
| Licences of everything shipped | **done** | The zip's `_nds/dscheevos/licenses/` has DScheevos (GPL-3.0), rcheevos, mbedTLS, lwIP, nds-bootloader, nds-bootstrap, and the toolkit libraries linked into the binaries: BlocksDS libnds, DSWiFi, start-up code and picolibc (launcher); devkitARM newlib (module), libnds 1.8 and libfat (nds-bootstrap), libnds 2 and calico (`load.bin`). The toolkit texts are vendored in `release/licenses/`; `SOURCES.txt` says which binary links what. |
| GPL obligations for shipped binaries | **done** | DScheevos, nds-bootstrap (GPL-3.0) and nds-bootloader (GPL-2.0+): `SOURCES.txt` names the exact commits, and the zip carries each one's licence text. |
| Privacy policy | **done** | `docs/privacy.md`, shipped as `privacy.txt`: what goes to RetroAchievements (username, token, game hash, unlocks, leaderboard entries, rich presence), the Wi-Fi key kept in RAM only, every file on the SD card including the offline queue, how to delete it, contact through the issue tracker. |

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

## Targets

1. **DSi / DSi XL** (DS games in DS mode): the first target.
2. **3DS**: second. There, the DS picture passes through TwlBg (the 3DS program that scales it), which can be patched (TWPatch does). Drawing the overlay there touches nothing the game uses and has room for badge images, so the 3DS version can look closer to RetroArch's.
3. **DS via flashcard** (B4DS mode): later, and a separate design rather than a setting: 4 MB RAM and no DSi extra RAM for the module, no WPA2 Wi-Fi, and nds-bootstrap's DScheevos hook refuses flashcards today (`arm9/source/dscheevos_session.c`).

## What's left, in order

1. On-screen notices during play. Built so far without touching the game: the pre-boot card with the mode and the Wi-Fi LED cues. Anything drawn over the game needs the owner's decision and RetroAchievements' answer first (`docs/overlay-research.md`): on the DSi nothing can draw over the game without touching its display state.
2. Ask RetroAchievements (RAdmin) to validate the client; the questions are in `docs/overlay-research.md`.
3. Real hardware: the tests listed in `docs/handoff.md` and `docs/overlay-research.md`.
4. A first public release, which starts the 6-month clock.
