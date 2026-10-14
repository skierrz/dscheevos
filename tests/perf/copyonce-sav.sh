#!/bin/sh
# SD_SETUP helper for copyonce.sh: copies the save file in $SAVE onto the emulated SD card's game save, so the
# run starts from a game with progress (achievements whose conditions the save already meets fire on load).
# Usage: SAVE=/path/to/game.sav SD_SETUP=tests/perf/copyonce-sav.sh tests/perf/copyonce.sh ...
cp "${SAVE:?}" "$1/games/game.sav"
