# TWiLight Menu++ end-to-end test

Starts a game the way a user does, in the headless melonDS runner (DSi mode, Wi-Fi through libslirp):

TWiLight Menu++ → A on the game → DScheevos launcher (installed as `_nds/nds-bootstrap-release.nds`) → HTTPS session on the mock server → chainload → DScheevos nds-bootstrap → game. Then it reads the module's status from a RAM dump.

It needs your own BIOS, firmware and NAND dumps and your own game dump, so it runs locally, not in CI. TWiLight Menu++ is downloaded at test time by `get-twilight.sh` (pinned to v27.24.1, `TWiLightMenu-DSi.7z`, checked against a SHA-256, cached in `~/.cache/dscheevos`). Nothing from it is kept in the repo.

```sh
DSI_FILES=~/dsi-private ROM=~/roms/kirby.nds \
BOOTSTRAP=../nds-bootstrap/bin/nds-bootstrap-nightly.nds RUNNER=path/to/melonds-headless \
HASH=6d77d9f9b34323e4060206b675620b8d SHOTS="1080 1500 2100" \
tests/twilight/run.sh tests/bootstrap/sets/pipeline.json 4000 --expect 9901 --never 9902
```

It passes when:

- TWiLight wrote `NDS_PATH = sd:/roms/nds/<game>` to `_nds/nds-bootstrap.ini`,
- the mock server saw `startsession` (with `HASH`, only for that ROM hash, so the launcher hashed the game TWiLight picked),
- nds-bootstrap used the session (`_nds/dscheevos/session.used`),
- `status.py` passes on the RAM dump.

`SHOTS` takes screenshots (PNG if Python has PIL) at those frames, each in its own run from a fresh SD. With the defaults: 1080 the TWiLight menu, 1500 the launcher, 2100 nds-bootstrap, and the main run's last frame is the game. Each run's SD, logs and screenshot stay in `WORK/<run>/`.

## The SD card

The DSi package as its install guide says (`BOOT.NDS`, `_nds/`, `roms/`), the game in `roms/nds/`, and DScheevos over it, as in `tests/launcher/run.sh`. TWiLight's first-run questions are answered in a seeded `_nds/TWiLightMenu/settings.ini`:

| Key | Why |
|---|---|
| `LANGUAGE_SET = 1`, `REGION_SET = 1` | skip the first-boot language and region screens |
| `DONT_SHOW_CLUSTER_WARNING = 1` | melonDS's SD image doesn't use 32 KB clusters; this is what "X: Don't show again" saves |
| `DSI_SPLASH = 0`, `SHOWLOGO = 0` | boot straight to the menu |
| `THEME = 0`, `SHOW_DIRECTORIES = 0`, `SHOW_BOX_ART = 0` | DSi theme with the game as the first and only icon |
| `ROM_FOLDER = sd:/roms/nds` | the game list |
| `BOOTSTRAP_FILE = 0` | nds-bootstrap "release", which is the launcher |

`PRESS_A` (default frame 1100) presses A once the menu is up (about frame 1000).

## Starting from Unlaunch

`BOOT=unlaunch` cold-boots through the DSi boot ROM instead of loading `BOOT.NDS` directly: boot ROM → Unlaunch → `sdmc:/BOOT.NDS` → the same chain. It needs a NAND copy with Unlaunch installed. `unlaunch-nand.sh` makes one from your `dsinand.bin` and only writes the copy:

```sh
DSI_FILES=~/dsi-private RUNNER=path/to/melonds-headless \
MELONDS_SRC=path/to/melonDS MELONDS_BUILD=path/to/melonds-headless-build \
tests/twilight/unlaunch-nand.sh /tmp/nand-unlaunch.bin

BOOT=unlaunch UNLAUNCH_NAND=/tmp/nand-unlaunch.bin DSI_FILES=... ROM=... BOOTSTRAP=... RUNNER=... \
tests/twilight/run.sh tests/bootstrap/sets/pipeline.json 4000 --expect 9901 --never 9902
```

It downloads Unlaunch v2.0 (freeware by Martin Korth, [problemkaputt.de/unlaunch.htm](https://problemkaputt.de/unlaunch.htm), pinned SHA-256), then installs it the way that page describes for a hardmod or no$gba: `UNLAUNCH.DSI` appended to the launcher's `title.tmd`. `nandtool.cpp` reads and writes that file through melonDS's `DSi_NAND` and is built against the runner's melonDS build. Running `UNLAUNCH.DSI` directly in melonDS doesn't work (it hits an undefined instruction at 0x4400). Then the script boots the copy once and, in Unlaunch's OPTIONS, sets NO BUTTON to `sdmc:/BOOT.NDS` and saves, as a user would. During that boot the SD holds only `BOOT.NDS`, so it is the last entry in Unlaunch's list whatever DSiWare the NAND has. Unlaunch stores the choice in the same `title.tmd`, and the script checks it there.

## Frame count

4000 frames in total gives the game about 1000. Longer runs of Kirby Super Star Ultra fill the module's 60 snapshot slots somewhere between 1800 game frames (none dropped) and 3000 (174 dropped), and `status.py` fails on dropped frames. That happens without TWiLight too: `tests/bootstrap/run.sh ... 4800` drops 521.
