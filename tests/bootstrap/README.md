# Bootstrap end-to-end test

Boots a retail game through the DScheevos build of nds-bootstrap in the headless melonDS runner (DSi mode), with a session file made by `tools/mksession`, then reads the module's status from a RAM dump.

It needs your own BIOS, firmware and NAND dumps and your own game dump, so it runs locally, not in CI:

```sh
DSI_FILES=~/dsi-private ROM=~/roms/kirby.nds \
BOOTSTRAP=../nds-bootstrap/bin/nds-bootstrap-nightly.nds RUNNER=path/to/melonds-headless \
tests/bootstrap/run.sh 6d77d9f9b34323e4060206b675620b8d tests/bootstrap/sets/pipeline.json 1800 \
  --expect 9901 --never 9902
```

`sets/pipeline.json` works with any game: 9901 needs 300 evaluated frames, 9902 never unlocks. Its GameId is Kirby Super Star Ultra's; the module doesn't check it.

Optional: `SCRIPT=file` for timed input (`FRAME KEYMASK HOLDFRAMES` lines), `WORK=dir` to keep the SD folder, screenshot and RAM dump.
