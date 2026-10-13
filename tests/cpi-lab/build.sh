#!/bin/sh
# Builds tests/cpi-lab/out/cpi-lab.nds (run from the repo root):
#   docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 tests/cpi-lab/build.sh
set -e
OUT=${LAB_OUT:-$PWD/tests/cpi-lab/out}   # LAB_OUT and extra make arguments: build variants
mkdir -p "$OUT"
export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
python3 tools/rcheevos-footprint/harness/gen_set.py 60 8 1 3 > "$OUT/set-typical.h"
make -s -C dscheevos/module -f Makefile -f ../../tests/cpi-lab/lab.mk BUILD="$OUT" "$@" "$OUT/cpi-lab.nds"
