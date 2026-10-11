#!/bin/bash
# Run built timing tests in the headless melonDS runner and print the RESULT lines.
#   MELONDS_HEADLESS=path/to/melonds-headless
#   DSI_FILES=dir with biosnds7/9.bin, dsfirmware.bin, biosdsi7/9.bin, dsifirmware.bin, dsinand.bin
#   RUNNER_ARGS="--cache-model"   extra runner flags (optional)
#   ./run.sh [out/timing-*.nds ...]
set -e
: "${MELONDS_HEADLESS:?set MELONDS_HEADLESS}" "${DSI_FILES:?set DSI_FILES}"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
roms=("$@"); [ ${#roms[@]} -eq 0 ] && roms=(out/timing-*.nds)
for rom in "${roms[@]}"; do
  modes="dsi"; [[ $rom == *-67.nds ]] && modes="ds dsi"
  for mode in $modes; do
    args=(--rom "$rom" --bios-dir "$DSI_FILES" --mode "$mode" --until DONE --frames 100000 $RUNNER_ARGS)
    if [ "$mode" = dsi ]; then cp "$DSI_FILES/dsinand.bin" "$work/nand.bin"; args+=(--nand "$work/nand.bin"); fi
    echo "== $(basename "$rom" .nds) on $mode"
    "$MELONDS_HEADLESS" "${args[@]}" 2>/dev/null | grep -E "^(START|LOAD|RESULT|UNLOCKS|FAIL)"
  done
done
