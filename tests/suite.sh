#!/bin/bash
# Serial regression suite (12 items): core and module host tests, the module test ROM, twilight (Kirby,
# Pokemon), WiFi drop, offline journal (2 runs), hardcore online, spectator, LED cues, real-set check.
# Every emulator run goes through emu-lock + hardtimeout; all work dirs are private (tests/lib/workdir.sh)
# and deleted on exit. Results: one "== item: result" line each, also in $SUITE_OUT/summary.txt.
#   SUITE_OUT=dir   keep the per-item logs there (default: a private work dir, deleted on exit)
#   LABEL           emu-lock label (default: suite)
# Needs the env of ../dscheevos.env (DSI_FILES, ROM*), RUNNER and BOOTSTRAP for this checkout, built module,
# launcher and tests/module ROM. Machine-wide docker builds go through build-lock.
here=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$here/.." && pwd)
. "$here/lib/workdir.sh"
cd "$W" || exit 1
export EMU_SLOTS=${EMU_SLOTS:-6}
LABEL=${LABEL:-suite}
dsc_softterm
WD=
if [ -n "${SUITE_OUT:-}" ]; then SP=$SUITE_OUT; mkdir -p "$SP"; own=; else SP=$(dsc_mkwork dsc-suite-$$); own=$SP; fi
trap '[ -z "$WD" ] || dsc_rmwork "$WD"; [ -z "$own" ] || dsc_rmwork "$own"' EXIT
E="$HOME/Developer/DScheevos/bin/emu-lock -l $LABEL $W/tests/perf/hardtimeout 900"
res() { echo "== $1: $2" | tee -a "$SP/summary.txt"; }
last() { grep -E "^RESULT|PASSED|checks," "$1" | tail -1; }

make -C dscheevos/core test > "$SP/core.txt" 2>&1; res core "$(last "$SP/core.txt")"
make -C dscheevos/module/tests test > "$SP/wpa.txt" 2>&1; res module-host "$(grep -E 'checks|layout' "$SP/wpa.txt" | tail -2 | tr '\n' ' ')"
$HOME/Developer/DScheevos/bin/build-lock -l "$LABEL" docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkitarm:20241104 make -C tests/module > "$SP/modrom-build.txt" 2>&1
cp "$DSI_FILES/dsinand.bin" "$SP/nand.bin"
$E "$RUNNER" $(dsc_runner_flags) --mode dsi --rom tests/module/out/module-test.nds --bios-dir "$DSI_FILES" --nand "$SP/nand.bin" --frames 3000 --until RESULT > "$SP/modrom.txt" 2>&1
rm -f "$SP/nand.bin"; res module-rom "$(last "$SP/modrom.txt")"

$E tests/twilight/run.sh tests/bootstrap/sets/pipeline.json 4000 --expect 9901 --never 9902 > "$SP/tw-kirby.txt" 2>&1; res twilight-kirby "$(last "$SP/tw-kirby.txt")"
ROM="$ROM_POKEMON" $E tests/twilight/run.sh tests/bootstrap/sets/pipeline.json 4000 --expect 9901 --never 9902 > "$SP/tw-pokemon.txt" 2>&1; res twilight-pokemon "$(last "$SP/tw-pokemon.txt")"
SCRIPT=tests/launcher/wifidrop.txt $E tests/launcher/run.sh tests/bootstrap/sets/live.json 6000 --expect-sent 4 --expect-rejoins 2 > "$SP/wifidrop.txt" 2>&1; res wifidrop "$(last "$SP/wifidrop.txt")"

# the two offline runs share one work dir (the second reads the journal the first left)
WD=$(dsc_mkwork dsc-suite-off-$$)
printf '4500 0x284 20\n4800 0x2 5\n' > "$SP/menu.txt"
MOCK_ARGS="--fail-awards --known-client" SCRIPT="$SP/menu.txt" WORK=$WD $E tests/launcher/run.sh tests/bootstrap/sets/live.json 5200 --expect-journal-unsent 4 > "$SP/offline1.txt" 2>&1; res offline-1 "$(last "$SP/offline1.txt")"
MOCK_ARGS=--known-client WORK=$WD $E tests/launcher/run.sh tests/bootstrap/sets/live.json 2600 > "$SP/offline2.txt" 2>&1
res offline-2 "$(last "$SP/offline2.txt"); $(grep -ho 'pending: sent [0-9]* unlocks' "$WD"/*.log "$WD"/sd/*.log 2>/dev/null | head -1)"
dsc_rmwork "$WD"; WD=

HARDCORE=1 LIVE=0 $E tests/launcher/run.sh tests/bootstrap/sets/live.json 6000 --expect-sent 4 > "$SP/hc-online.txt" 2>&1; res hardcore-online "$(last "$SP/hc-online.txt")"
SPECTATOR=1 $E tests/launcher/run.sh tests/bootstrap/sets/live.json 6000 > "$SP/spectator.txt" 2>&1; res spectator "$(last "$SP/spectator.txt")"
RUNNER_ARGS=--trace-led $E tests/launcher/run.sh tests/bootstrap/sets/cues.json 6000 > "$SP/cues.txt" 2>&1; res cues "$(last "$SP/cues.txt"); led lines $(grep -c '\[led\]' "$SP/cues.txt")"
$E tests/real/run.sh --check > "$SP/real.txt" 2>&1; res real-check "exit $? $(tail -1 "$SP/real.txt")"
echo SUITE DONE | tee -a "$SP/summary.txt"
