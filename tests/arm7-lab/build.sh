#!/bin/sh
# Builds tests/arm7-lab/out[-variant]/arm7-lab.nds (run from the repo root):
#   docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 tests/arm7-lab/build.sh [VAR=value...]
# e.g. BUILD=tests/arm7-lab/out-thumbO2 RC_ISA=thumb RC_OPT=-O2 OPT=-O2 FAST=hot
set -e
export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
for a in "$@"; do case $a in BUILD=*) B=${a#BUILD=};; esac; done
B=${B:-tests/arm7-lab/out}
mkdir -p "$B"
python3 tools/rcheevos-footprint/harness/gen_set.py 60 8 1 3 > "$B/set-typical.h"
make -s -f tests/arm7-lab/Makefile "$@"
