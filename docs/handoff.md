# Handoff: cloud session → local Claude Code (Oct 2026)

Everything below is pushed to GitHub. Nothing lives only in the cloud container except your private files (BIOS, firmware, NAND, ROMs) and build outputs, which you rebuild.

## Repositories and branches

| Repo | Branch | State |
|---|---|---|
| `skierrz/dscheevos` | `main` | last release build: `090b4ae` |
| | `snapshot-repeats` | PR #12, CI green, parked |
| | `backlog-reads` | on top of #12: queued frames use memory from their own VBlank. CI green. No PR yet. |
| | `compliance` | on top of `backlog-reads`: GPL-3.0 `LICENSE`, `VERSION` file, `docs/compliance.md`, hardcore warning detection. Newest work, `fb72f3d`. No PR yet. |
| `skierrz/nds-bootstrap` | `dscheevos` | what `main` builds with (`9c602ce`) |
| | `dscheevos-hc-status` | `9deddf1`, in-game page shows whether RA counts hardcore. `compliance` points its submodule here. Fast-forward `dscheevos` to it when `compliance` merges (the release README and `SOURCES.txt` name the `dscheevos` branch). |
| `skierrz/melonDS` | `dscheevos` | `e9dd6eb`, test bench only (`tools/melonDS` submodule) |

The three dscheevos branches are stacked: #12 must merge first (or rebase the stack onto `main`). Suggested order: merge #12, then open PRs for `backlog-reads` and `compliance` in turn.

## Setting up on a Mac

1. `git clone --recurse-submodules https://github.com/skierrz/dscheevos && cd dscheevos && git checkout compliance && git submodule update --init --recursive`
2. Docker Desktop. The images are x86-64; on Apple Silicon they run under emulation: slower, but they work. Images: `devkitpro/devkitarm:20241104` (module, nds-bootstrap), `devkitpro/devkitarm:20260610` (loader), `skylyrac/blocksds:slim-v1.24.0` (launcher). The exact commands are in `.github/workflows/release.yml`.
3. Native tools: `brew install cmake ninja openssl python` (and `pillow` for screenshots: `pip3 install pillow`).
4. Build the headless runner: `cmake -S tools/melonDS/tools/dscheevos/headless -B build/runner -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/runner`. Game tests: see `tests/games/README.md`.
5. Private files, never in git. Put them in one folder (e.g. `~/dsi-private`) and point `DSI_FILES` at it: `biosnds7.bin`, `biosnds9.bin`, `biosdsi7.bin`, `biosdsi9.bin`, `dsfirmware.bin`, `dsifirmware.bin`, `dsinand.bin` (your own dumps). ROMs: Kirby Super Star Ultra (USA), Pokemon Platinum (USA, Rev 1), New Super Mario Bros. (USA) from your Drive folder. TWiLight Menu++ downloads itself (`tests/twilight/get-twilight.sh`).

Checks that should pass before you change anything:
- `make -C dscheevos/core test`: 130 checks
- `tests/games/run.sh` (homebrew ROMs via `tests/games/fetch_roms.py`): all pass, idle every frame and every 600 frames
- `tests/twilight/run.sh tests/bootstrap/sets/pipeline.json 4000 --expect 9901 --never 9902` with `DSI_FILES`, `ROM`, `BOOTSTRAP` (nds-bootstrap built from `external/nds-bootstrap`), `RUNNER`: RESULT PASS, `'hardcore': 2`

Practical notes from the cloud runs: a full TWiLight chain run takes 1–3 minutes; old run folders hold NAND and SD copies (hundreds of MB each), so clean `WORK` dirs.

## Open work, in order

1. **SD unlock queue** (compliance A, "offline queueing"). The module keeps pending unlocks and leaderboard entries in RAM and writes them only when it's safe (not from the VBlank IRQ: another fork froze Ketsui doing that). Candidates: when the in-game menu opens, and at exit. The launcher sends them at the next start, with rcheevos' `o=` offset. Also port their fix for nds-bootstrap's `fileWrite` partial-sector bug (`retail/common/source/my_fat.c`, see `bakkerrrs/retroachievements-ds-on-3ds` docs, "fileWrite") before writing files in-game.
2. **Full achievement list** in the in-game menu: titles, descriptions, state, measured progress.
3. **Overlay during play** (compliance A and E). See below.
4. **Rich presence in hardcore**: keep the network path on while a hardcore session runs (now it only runs with "Send unlocks during play").
5. **Privacy policy**; ship DScheevos' own licence and the toolkit licences (libnds, DSWiFi, newlib) in the zip.
6. **VBlank cost**: about 113k instructions per frame for a typical synthetic set (qemu count), up from 54k before `backlog-reads`. Room to cut: read chain pointers from the snapshot instead of calling the platform reader; fill small gaps between table runs.
7. **Ask RA** (RAdmin) about approving a hardware client: hardcore validation, how they want unlocks offset by days treated, rich presence without pings, and what "displayed during gameplay" means for a DSi. Their FAQ says manual unlocks aren't given for "original hardware", which hints at how they see hardware clients today.
8. **First public release**: starts RA's 6-month clock.
9. **Hardware test** when you're back with the DSi: watch the router's client list at boot, +5, +15 and +30 minutes into a game. Another fork (3DS) lost Wi-Fi seconds after the game booted; we switch the Wi-Fi hardware differently, but that's untested.

## Overlay research in progress

The owner's rules: no pausing, no slowdown, no corruption, no way to change game state or show more than RA allows. Graphical, RetroArch-like if possible; badge images a stretch goal.

- 3DS (later): patch TwlBg and draw after the DS picture is finished. Zero contact with the game. Noted in `docs/compliance.md`.
- DSi: a 4-ideator → 4-critic → synthesis workflow was started and stopped early (cloud credits). `docs/handoff-overlay-gauntlet.js` is the script; it has absolute paths from the cloud box (`/home/user/...`, `/tmp/claude-0/gbatek`) to change before rerunning. It needs: this repo, `~/nds-bootstrap` (fork, `dscheevos-hc-status`), a clone of `Bakkerrrs/retroachievements-ds-on-3ds`, and GBATEK as text (problemkaputt.de/gbatek.htm). In Claude Code: ask it to "run the workflow in docs/handoff-overlay-gauntlet.js".
- The one ideator that finished (VRAM/display-mode angle) is in `docs/handoff-overlay-ideas-vram.json`. Its lead idea: a banner on the top scanlines in engine A's main-memory display mode, fed by a DSi-only NDMA channel from DScheevos' RAM. It rejected display-capture blending, windows/blending, master brightness, extended palettes, VRAM H/I and the 3D quad. None of this has been checked by a critic yet.

## What the RA docs say (sources)

- [Hardcore Compliance Requirements](https://docs.retroachievements.org/general/hardcore-compliance-requirements.html): the checklist, tracked in `docs/compliance.md`.
- [rcheevos rc_client integration](https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration): unknown user agent → hardcore filed as softcore; contact RAdmin to be validated. Unlocks are retried automatically.
- [RA FAQ](https://docs.retroachievements.org/general/faq.html): resetting progress per game (game page or settings) and per achievement.
- [Getting started as an achievement developer](https://docs.retroachievements.org/developer-docs/getting-started-as-an-achievement-developer.html): set authors test with Local and Unofficial sets in RAIntegration (Windows).
- For bench runs on a real account: rc_client spectator mode (`rc_client_set_spectator_mode_enabled`) evaluates without sending unlocks.
