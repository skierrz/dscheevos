# DScheevos

RetroAchievements for DS games on a DSi (and 3DS in DSi mode), on the console itself, without emulation. Games start from TWiLight Menu++ through a DScheevos build of nds-bootstrap; achievements are checked every frame in the game's idle time.

> **Status: test builds, not yet tried on a real DSi.** In the emulator test bench, the whole flow works: session start over HTTPS, per-frame evaluation, unlocks sent during play, the in-game achievements page and the hardcore lockouts. Dropped Wi-Fi rejoins by itself, with the module's own WPA2 handshake. The open item is testing on real hardware (see `docs/architecture.md`). See `release/README.txt` for what a build can do.

**Cost:** free. No paid features, no ads, and no donations required.

**Privacy:** DScheevos talks only to RetroAchievements and collects nothing for itself. What it sends and stores: [`docs/privacy.md`](docs/privacy.md).

## How it fits together

| Piece | Where | What it does |
|---|---|---|
| Launcher | `dscheevos/launcher` | Runs when a game starts. It logs in, hashes the ROM and starts the session over HTTPS, writes `session.bin`, then boots nds-bootstrap. |
| Module | `dscheevos/module`, `dscheevos/core` | Runs inside the game. It snapshots watched memory at VBlank and evaluates with rcheevos in idle time. |
| nds-bootstrap hooks | [skierrz/nds-bootstrap](https://github.com/skierrz/nds-bootstrap) (`dscheevos` branch, `external/nds-bootstrap`) | Loads the module and calls it at VBlank, at `OS_Halt` and on reset. Adds the in-game achievements page and the hardcore lockouts. |
| Test bench | [skierrz/melonDS](https://github.com/skierrz/melonDS) (`dscheevos` branch, `tools/melonDS`) | Headless runner with an ARM9 cache timing model and Wi-Fi. Development only: DScheevos itself never needs an emulator. |

Design: `docs/architecture.md`. Measurements: `docs/rcheevos-arm9-footprint.md`. Hardware notes: `docs/hardware-access.md`, `docs/original-ds-and-flashcarts.md`.

## Building

`.github/workflows/release.yml` builds everything and packs the SD card zip (`release/package.sh`); every push to main uploads it as an artifact, and a `v*` tag publishes a pre-release. Each part also builds on its own with Docker (see the READMEs in `dscheevos/launcher` and `dscheevos/module`).

## Tests

- `dscheevos/core`: unit tests (`make -C dscheevos/core test`)
- `tests/games`: homebrew games with RetroAchievements-style sets, checked frame by frame against rcheevos' reference
- `tests/module`: the module in DSi mode in melonDS
- `tests/bootstrap`, `tests/launcher`: retail games through nds-bootstrap, and the full launcher chain. These need your own firmware and game dumps, so they run locally.

rcheevos is used unmodified as a submodule.

## Licence

GPL-3.0 (`LICENSE`), the same as nds-bootstrap, which the module runs inside. rcheevos (MIT), lwIP (BSD) and mbedTLS (Apache-2.0) keep their own licences. The release zip carries every licence of what it ships, toolkit libraries included, in `_nds/dscheevos/licenses/`.
