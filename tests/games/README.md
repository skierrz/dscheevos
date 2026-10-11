# Game tests

Plays real homebrew DS games in the emulator with scripted input and checks that DScheevos unlocks the right achievements at the right frames, in a hardcore session against a mock server.

Each frame, the harness runs the game, takes DScheevos' VBlank snapshot and evaluates it, and also runs a reference: rcheevos' runtime reading the same RAM directly, the way RetroArch does. Both must unlock the same achievements on the same frames. That catches DScheevos bugs independently of whether a test set is well written.

| Game | RA game | What the test set covers |
|---|---|---|
| Giana Sisters DS | 17940 | BCD timer, lives, delta and hit-count conditions, an achievement that must not fire |
| The Martian | 38533 | Pause flag, transitions, AndNext hit counts, a pointer chain (AddAddress) |
| Dicewars DS | 17821 | Menu cursor, a leaderboard (start, cancel, submit) |

The sets in `sets/` are written for these tests (addresses found by RAM search), in the same format the RetroAchievements server sends. They are not the official sets.

## Run

```sh
make -C tools/rahash                                  # RetroAchievements DS hasher
python3 tests/games/fetch_roms.py --hasher tools/rahash/rahash --out ~/dsc-roms
cmake -S tests/games -B build/games -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/games
DSC_GAMETEST=build/games/dsc-gametest DSC_ROMS=~/dsc-roms tests/games/run.sh
```

`fetch_roms.py` downloads the games from GameBrew and keeps only files whose hash matches what RetroAchievements lists. Game files never go in this repo.

## Adding a game

1. Find addresses: run `dsc-gametest --rom X --script S --dump 100,200 --dump-dir D` and compare the RAM dumps (main RAM, then DTCM).
2. Write a set in `sets/`, an input script in `scripts/`, and a line in `games.txt` with frame windows for each expected unlock.
