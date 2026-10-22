#!/bin/bash
# LAB ONLY: build the strip module (STRIP=1) into dscheevos/module/build-strip, in Docker.
# CFLAGS_EXTRA: compile flags (several allowed), e.g. "-DSTRIP_RELAXED -DSTRIP_ROWS=32".
set -eo pipefail
DS=$HOME/Developer/DScheevos; W=$(cd "$(dirname "$0")/../.." && pwd); N=$(basename "$W"); G=$DS/dscheevos/.git
make -C "$W/dscheevos/display" build/font_latin.c >/dev/null
timeout 900 "$DS/bin/build-lock" -l striplab docker run --rm --platform linux/amd64 -v "$W:/ws/$N" -v "$G:/ws/dscheevos/.git" -v "$G:$G" \
  -w "/ws/$N" devkitpro/devkitarm:20241104 make -C dscheevos/module STRIP=1 BUILD=${BUILD:-build-strip} ${MAKEARGS:-} "EXTRA_CFLAGS=${CFLAGS_EXTRA:-}" 2>&1 \
  | grep -E "error:|warning:|^built" || true
