# Feature-complete plan (ABI v6)

Goal: everything in `docs/compliance.md` "What's left" works end to end in the emulator test bench, unit tests pass, and the build is ready for a bench run on a real RetroAchievements test account (in melonDS). Real hardware comes after.

## Contract (frozen)

`dscheevos/module/include/dscheevos_module.h`, section "ABI v6", and the header layout in `dscheevos/module/src/start.s`. Changing it needs the integrator's OK, because the module, the launcher and nds-bootstrap all read it.

| Header word | What |
|---|---|
| 0x04 version | 6 (nds-bootstrap's `DSCHEEVOS_VERSION` must match) |
| 0x2C `list` | published `dscm_list_t*`, swapped whole after a rebuild in idle time; 0 until the set loads |
| 0x30 `notices` | `dscm_notices_t*`: ring of one-shot notices + challenge indicators + leaderboard trackers |
| 0x34 `journal` | `dscm_journal_t*` (cached-mirror address). The module writes it through the cache and cleans the lines at every step (`journal_sync`), so RAM is always consistent and the ARM7 can copy it with no cache work |

`dscm_boot_t.flags` (was `reserved[0]`, offset 0x1CE): `DSCM_BOOT_SPECTATOR`.

Journal file: `sd:/_nds/dscheevos/pending.bin`, exactly `DSCM_JOURNAL_FILE_SIZE` (4096) bytes, created by the launcher before it boots nds-bootstrap (zeroed). nds-bootstrap's front end finds its clusters like `session.bin`'s; the ARM7 card engine writes `DSCM_JOURNAL_FILE_SIZE` bytes from `header->journal` (zero-padded past `sizeof(dscm_journal_t)`) at safe points only: when the in-game menu opens, before "Reset Game" and "Exit", and before a soft reset (key combo). Never from a game VBlank/IRQ while the game runs (another fork froze games that way). nds-bootstrap's `fileWrite` partial-sector bug must be fixed first (see Bakkerrrs/retroachievements-ds-on-3ds docs, "fileWrite"; `retail/common/source/my_fat.c`).

## Work packages and file ownership

Agents share one checkout. Each owns its files; nobody else edits them. Nobody commits: the integrator does.

1. **Core and module** (`dscheevos/core/**`, `dscheevos/module/**` except the frozen header, `tests/module/**`, `tools/mksession/**`)
   - Journal: an entry per unlock and leaderboard submission (time = `dscm_unix_time()`); `sent = 1` when the server answers (success, or an error that won't be retried, e.g. already unlocked); `username`, `game_hash`, `game_id` filled at load.
   - List: built from rc_client (`rc_client_create_achievement_list`, core set, display order), rebuilt in idle when an unlock, progress, challenge or reset event marks it stale, at most once per 30 frames, double-buffered, published by swapping `dscm_list_ptr`.
   - Notices: from rc_client events (all of `RC_CLIENT_EVENT_*` that a client should show), session start (mode) once loaded, mastery, disconnected/reconnected.
   - Spectator flag → `rc_client_set_spectator_mode_enabled` (core config field).
   - Host unit tests for all of it in `dscheevos/core/tests`; the module test ROM (`tests/module`) checks list, notices and journal in RAM. `make -C dscheevos/core test` and the module test ROM must pass.
2. **Launcher and tools** (`dscheevos/launcher/**`, `tools/mock-ra-server/**`, `tests/launcher/**`, `tests/real/**` new)
   - Create `pending.bin` (4096 bytes, zeroed) if missing, before booting.
   - At start, once online and logged in: if `pending.bin` holds a journal for this user, send each unsent entry (award: `seconds_since_unlock`; leaderboard: `seconds_since_completion`), using rcheevos' rc_api request builders, then clear the file. Show "Sent N unlocks from last time". Already-unlocked replies count as sent.
   - Hardcore: rich presence and leaderboards can't be turned off. In hardcore the session always keeps Wi-Fi ("Send unlocks during play" is forced on and shown as such); softcore keeps the setting.
   - `SPECTATOR = 1` in `dscheevos.ini` (not in the menu): spectator mode in the launcher's rc_client and `DSCM_BOOT_SPECTATOR` in the session. The launcher shows "Spectator: nothing is sent".
   - Mock server: honour `o=` (log `offset`), `--fail-awards` (answer awards with a retryable 503), and keep `spectator` visible in the log (no award requests may arrive).
   - `tests/real/run.sh`: the launcher flow against the real server (`HOST` unset). Takes `RA_USER` and `RA_PASSWORD` (or `RA_TOKEN`), logs in from the host with curl to get the token (never stored in the repo), writes the ini, `SPECTATOR=1` by default (`LIVE_UNLOCKS=1` to really send), runs the game, prints the session's status. A `--check` mode with no account must show the launcher reaching retroachievements.org over TLS through melonDS (a login error from the real server, not a network error).
3. **nds-bootstrap** (`external/nds-bootstrap/**`, branch `dscheevos-features` off `dscheevos-hc-status`)
   - `DSCHEEVOS_VERSION 6`, hooks.h mirrors of the v6 structs and offsets.
   - Front end: `pending.bin` cluster; bootloader → card engine ARM7 (like `ramDumpCluster`).
   - ARM7: journal write command from the in-game menu, and before reset / exit / soft reset; `fileWrite` partial-sector fix.
   - In-game menu "Achievements..." page: full list with paging (title, points, state, measured progress, description of the selected one), the session summary that's there now, active challenges and trackers. Must fit the card engine size limits; build must pass.
4. **Release and docs** (`release/**`, `docs/privacy.md`, `README.md`, `docs/compliance.md`)
   - Privacy policy (what goes where: username, token, game hashes, unlocks to retroachievements.org; Wi-Fi settings read from the console, never stored; what's on the SD card; no analytics).
   - Zip ships DScheevos' `LICENSE`, the privacy policy, and the toolkit licences (BlocksDS libnds/DSWiFi, devkitARM libnds/newlib/calico as linked), vendored under `release/licenses/` so packaging needs no network.
   - Monetization statement in README.
5. **Overlay** (research first): how to show notices during play on a DSi without breaking the hard rules. Outcome is a decision for the owner, not code, unless a design meets every rule.

## End-to-end checks (integration)

All existing checks plus:
- Offline queue: mock server `--fail-awards`; play until unlocks; open the in-game menu (L+DOWN+SELECT) and close it; `pending.bin` holds the unsent unlocks; second launcher run with a working server sends them with `o=` > 0 and clears the file.
- Achievement list: in-game menu page screenshot shows the set; status RAM dump shows `list` with every achievement and the unlocked ones marked.
- Hardcore always online: with `SEND_DURING_PLAY = 0` and `HARDCORE = 1`, pings still reach the server.
- Spectator: no award requests reach the server; the in-game menu still lists the unlocks.
- Real server: `tests/real/run.sh --check` reaches retroachievements.org.
