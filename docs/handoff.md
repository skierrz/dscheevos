# Handoff: cloud session → local Claude Code (Oct 2026)

Everything below is pushed to GitHub. Nothing lives only in the cloud container except your private files (BIOS, firmware, NAND, ROMs) and build outputs, which you rebuild.

## Repositories and branches

| Repo | Branch | State |
|---|---|---|
| `skierrz/dscheevos` | `main` | last release build: `090b4ae` |
| | `snapshot-repeats` | PR #12, CI green, parked |
| | `backlog-reads` | on top of #12: queued frames use memory from their own VBlank. CI green. No PR yet. |
| | `compliance` | on top of `backlog-reads`: GPL-3.0 `LICENSE`, `VERSION` file, `docs/compliance.md`, hardcore warning detection. No PR yet. |
| | `mac-setup` | on top of `compliance`: macOS script fixes, `.gitignore`, Wi-Fi drop script, local setup notes. |
| | `features` | on top of `mac-setup`: ABI v6, offline queue, full achievement list, notices, hardcore always online, spectator mode, real-server bench, privacy policy, toolkit licences. Newest work. |
| `skierrz/nds-bootstrap` | `dscheevos` | what `main` builds with (`9c602ce`) |
| | `dscheevos-hc-status` | `9deddf1`, in-game page shows whether RA counts hardcore. `compliance` points its submodule here. |
| | `dscheevos-features` | on top of `dscheevos-hc-status`: ABI v6, journal writes at safe points, `fileWrite` fix, full achievement list page. `features` points its submodule here. Fast-forward `dscheevos` to it when `features` merges (the release README and `SOURCES.txt` name the `dscheevos` branch). |
| `skierrz/melonDS` | `dscheevos` | `e9dd6eb`, test bench only (`tools/melonDS` submodule) |
| | `dscheevos-features` | on top: in-place SD writes reach the host folder, `--rtc` (clock starts at host time). `features` points its submodule here. |

The dscheevos branches are stacked: #12 must merge first (or rebase the stack onto `main`). Suggested order: merge #12, then PRs for `backlog-reads`, `compliance`, `mac-setup` and `features` in turn.

## Setting up on a Mac

1. `git clone --recurse-submodules https://github.com/skierrz/dscheevos && cd dscheevos && git checkout compliance && git submodule update --init --recursive`
2. Docker Desktop. The images are x86-64; on Apple Silicon they run under emulation: slower, but they work. Images: `devkitpro/devkitarm:20241104` (module, nds-bootstrap), `devkitpro/devkitarm:20260610` (loader), `skylyrac/blocksds:slim-v1.24.0` (launcher). The exact commands are in `.github/workflows/release.yml`.
3. Native tools: `brew install cmake ninja openssl python` and `brew install pillow` for screenshots (`pip3` on a Mac is the system Python 3.9, not Homebrew's `python3`; the scripts skip PNG conversion silently without it).
4. Build the headless runner: `cmake -S tools/melonDS/tools/dscheevos/headless -B build/runner -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/runner`. Game tests: see `tests/games/README.md`.
5. Private files, never in git. Put them in one folder (e.g. `~/dsi-private`) and point `DSI_FILES` at it: `biosnds7.bin`, `biosnds9.bin`, `biosdsi7.bin`, `biosdsi9.bin`, `dsfirmware.bin`, `dsifirmware.bin`, `dsinand.bin` (your own dumps). ROMs: Kirby Super Star Ultra (USA), Pokemon Platinum (USA, Rev 1), New Super Mario Bros. (USA) from your Drive folder. TWiLight Menu++ downloads itself (`tests/twilight/get-twilight.sh`).

Checks that should pass before you change anything:
- `make -C dscheevos/core test`: 1763 checks
- `tests/games/run.sh` (homebrew ROMs via `tests/games/fetch_roms.py`): all pass, idle every frame and every 600 frames
- `tests/twilight/run.sh tests/bootstrap/sets/pipeline.json 4000 --expect 9901 --never 9902` with `DSI_FILES`, `ROM`, `BOOTSTRAP` (nds-bootstrap built from `external/nds-bootstrap`), `RUNNER`: RESULT PASS, `'hardcore': 2`

Practical notes from the cloud runs: a full TWiLight chain run takes 1–3 minutes; old run folders hold NAND and SD copies (hundreds of MB each), so clean `WORK` dirs.

## Local setup (macOS)

Set up and checked on an Apple Silicon Mac (macOS 26, Oct 2026). Everything below builds and every check passes.

**Layout** (one workspace folder, here `WS`; the repos never hold the private files):

| Path | What |
|---|---|
| `WS/` | workspace folder, not a git repo |
| `WS/dscheevos/` | this repo |
| `WS/nds-bootstrap/` | the fork, `dscheevos-hc-status`, for in-game work (the submodule is a separate checkout) |
| `WS/private/dsi/` | BIOS, firmware and NAND dumps (`DSI_FILES`) |
| `WS/private/roms/` | Kirby, Pokemon Platinum, NSMB |
| `WS/private/homebrew-roms/` | `fetch_roms.py` output (`DSC_ROMS`) |
| `WS/dscheevos.env` | exports `DSI_FILES`, `ROM_KIRBY`, `ROM_POKEMON`, `ROM_NSMB`, `ROM` (Kirby), `DSC_ROMS`, `RUNNER`, `BOOTSTRAP`, `DSC_GAMETEST`. From the repo: `source ../dscheevos.env` |

**Homebrew packages** beyond step 3 above: `pkgconf` and `enet` (the runner's CMake looks up libenet with pkg-config), and `sevenzip` for `get-twilight.sh` (it installs `7zz`, which the script accepts).

**Docker on Apple Silicon:** pass `--platform linux/amd64` to every `docker run`. Under emulation nds-bootstrap takes about 12 minutes and the module about 2. The devkitARM image has no host gcc, so build `lzss` for Linux in the `gcc` image and mount it as CI does:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src gcc:14 \
  gcc -O2 -static external/nds-bootstrap/lzss.c -o build/host/lzss
docker run --rm --platform linux/amd64 -v "$PWD:/src" -v "$PWD/build/host/lzss:/usr/local/bin/lzss:ro" \
  -w /src/external/nds-bootstrap devkitpro/devkitarm:20241104 \
  bash -c 'git config --global safe.directory "*" && make package-nightly'
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 make -C dscheevos/module
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20260610 make -C dscheevos/launcher/loader
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src skylyrac/blocksds:slim-v1.24.0 make -C dscheevos/launcher
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 make -C tests/module
release/package.sh "$PWD/out/dscheevos" "$(git describe --tags --always)"
```

**Checks** (with the env file sourced; times on this Mac):

| Check | Result |
|---|---|
| `make -C dscheevos/core test` | 1763 checks, 0 failed |
| `make -C dscheevos/module/tests test`, session layout (as in `test.yml`) | 54 checks, 0 failed; layout OK |
| `tests/games/run.sh` | ALL GAME TESTS PASSED (2m40s) |
| module test ROM: `$RUNNER --mode dsi --rom tests/module/out/module-test.nds --bios-dir "$DSI_FILES" --nand COPY --frames 3000 --until RESULT` | RESULT PASS |
| `tests/twilight/run.sh tests/bootstrap/sets/pipeline.json 4000 --expect 9901 --never 9902`, Kirby | RESULT PASS, `'hardcore': 2` (30 s) |
| same, `ROM="$ROM_POKEMON"` | RESULT PASS, `'hardcore': 2` |
| `SCRIPT=tests/launcher/wifidrop.txt tests/launcher/run.sh tests/bootstrap/sets/live.json 6000 --expect-sent 4 --expect-rejoins 2`, Kirby | RESULT PASS, 4 sent, 2 rejoins (45 s) |

**Gotchas:**
- `--nand` gets modified: always pass a copy, never `$DSI_FILES/dsinand.bin`.
- Run emulator tests one at a time (they share mock-server port 8443) and set `WORK` to a folder you delete afterwards: 450–650 MB per run.
- macOS's `/sbin/sha256sum` has no `--quiet`, and Debian's `/usr/share/common-licenses/GPL-2` doesn't exist on a Mac: the scripts no longer depend on either (branch `mac-setup`).
- The runner's CMake defines `ARCHITECTURE_x86_64=1` even on arm64. Only JIT code reads it and the runner builds without JIT, so it's harmless; turn JIT on and it won't be.
- In zsh, `set -- $var` doesn't split words; wrap loops like that in `bash -c`.

## Open work, in order

Done on `features` (Oct 2026): offline queue, full achievement list, rich presence in hardcore, privacy policy and licences, spectator mode, real-server bench, pre-boot placard with the mode, Wi-Fi LED cues (`RUNNER_ARGS=--trace-led` shows them; `tests/bootstrap/sets/cues.json` has measured, challenge and leaderboard entries). Details in `docs/compliance.md` and `docs/architecture.md`.

1. **Notices drawn during play** (compliance A and E): needs the owner's decision first; the LED cues and the placard are the safe part, already built. The LED cue style (dark pulses on a lit LED) is not signed off yet. `docs/overlay-research.md` has the options, evidence from melonDS experiments, the decisions, the questions for RetroAchievements and the hardware tests. The module already produces the notices (`dscm_notices_t`); only the display is missing.
2. **Ask RA** (RAdmin) about approving a hardware client: the questions are in `docs/overlay-research.md`. Their FAQ says manual unlocks aren't given for "original hardware", which hints at how they see hardware clients today.
3. **Bench run on a real test account** in melonDS: `tests/real/run.sh` (spectator by default; `tests/real/README.md`).
4. **VBlank cost**: the snapshot is now taken when the game halts (idle time, IRQs masked in short chunks); the VBlank IRQ only queues it, about 60 instructions (qemu, `tests/perf`; it was 120k). melonDS A/B: lag equal to stock in Kirby and Pokemon (`tests/perf/AB.md`). Still open: evaluation of the synthetic typical set needs about a frame of CPU per frame, so busy games drop frames (Phase 4, evaluation speed); real sets may be lighter. Next: evaluation speed, then a hardware timing run.
5. **Hardware test** with the DSi: watch the router's client list at boot, +5, +15 and +30 minutes into a game (another fork lost Wi-Fi seconds after boot on 3DS); open the in-game menu after unlocks with the server unreachable and check `pending.bin` is sent at the next start; the overlay hardware tests in `docs/overlay-research.md`.
6. **rcheevos bug to report upstream** (v12.5.0 and master): `rc_client_unload_game` clears `client->game` and then raises a pending mastery event, which dereferences it. Mastery is pending from the server's answer to the last unlock until the next frame, so quitting right then crashed. The core marks a pending mastery as shown before unloading (`dsc_shutdown`); `test_shutdown_right_after_mastery` covers it.
7. **First public release**: starts RA's 6-month clock.

## End-to-end checks on `perf`

All with the env file sourced, one at a time, WORK deleted after each. Results from the last run (Oct 2026): all pass.

| Check | Command |
|---|---|
| Core | `make -C dscheevos/core test` (1763 checks: every snapshot test twice, copy in the IRQ and at halt) |
| Module test ROM | as in "Local setup"; checks list, notices, journal |
| Offline queue | `MOCK_ARGS="--fail-awards --known-client" SCRIPT=<open and close the menu at 4500/4800: lines "4500 0x284 20", "4800 0x2 5"> tests/launcher/run.sh tests/bootstrap/sets/live.json 5200 --expect-journal-unsent 4`, then with the same WORK: `MOCK_ARGS=--known-client tests/launcher/run.sh tests/bootstrap/sets/live.json 2600` (launcher.log: "pending: sent 4 unlocks"; no unlock twice; `offset` > 0 for the earlier unlocks: melonDS runs faster than real time, so the last ones can be stamped ahead of the host clock and are sent with 0) |
| Still offline at the next start | the first run twice, then the normal one: the second sets the old journal aside in `unsent/`, the third sends all 8 |
| Hardcore always online | `HARDCORE=1 LIVE=0 tests/launcher/run.sh tests/bootstrap/sets/live.json 6000 --expect-sent 4` (a `ping` reaches the server) |
| Spectator | `SPECTATOR=1 tests/launcher/run.sh tests/bootstrap/sets/live.json 6000` (no `startsession` or `unlock` events; the list shows the unlocks) |
| Real server | `tests/real/run.sh --check` (no account: retroachievements.org answers over TLS) |
| In-game list | open the menu, DOWN, A (summary), RIGHT (list), A (detail); screenshots with the runner's `--screenshot` |

## What the RA docs say (sources)

- [Hardcore Compliance Requirements](https://docs.retroachievements.org/general/hardcore-compliance-requirements.html): the checklist, tracked in `docs/compliance.md`.
- [rcheevos rc_client integration](https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration): unknown user agent → hardcore filed as softcore; contact RAdmin to be validated. Unlocks are retried automatically.
- [RA FAQ](https://docs.retroachievements.org/general/faq.html): resetting progress per game (game page or settings) and per achievement.
- [Getting started as an achievement developer](https://docs.retroachievements.org/developer-docs/getting-started-as-an-achievement-developer.html): set authors test with Local and Unofficial sets in RAIntegration (Windows).
- For bench runs on a real account: rc_client spectator mode (`rc_client_set_spectator_mode_enabled`) evaluates without sending unlocks.
