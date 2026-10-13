#!/bin/sh
# Hardcore lockdown reproducers: files a player can put on the SD card that
# would change the game after the launcher hashed the clean ROM. Each case
# boots a game in melonDS (tests/bootstrap/run.sh: nds-bootstrap with a
# mksession session; tests/launcher/run.sh: the whole launcher flow) and
# prints what it observed. Run it once with the build under test and once
# with an older BOOTSTRAP (and LAUNCHER) to compare.
#
# Most cases write the marker "DSCX" over bytes 4-7 of the ARM9 binary
# (0x02000004: the secure area's ID, never executed) and read it back from
# the RAM dump: "applied" means the SD file changed the game.
#
# Needs DSI_FILES, ROM (Kirby Super Star Ultra (USA)), BOOTSTRAP, RUNNER as the
# other tests do. Kirby has a fix in nds-bootstrap's apfix.pck (cheat form),
# so a user apFix file competes with it; ROM_USERONLY (a game with none, e.g.
# New Super Mario Bros. (USA)) is used by launcher-ips. SRL (any small .nds,
# e.g. tests/module/out/module-test.nds) for srl.
# Optional: LAUNCHER (default: the repo's), MODE=hc|casual (default hc).
# Usage: lockdown.sh CASE...   cases: ips bin widecheat srl cache stale wipe
#                                     launcher-ips launcher-builtin launcher-donor
#                                     boost dsimode2 boost-excluded (speed-ups: ROM_EXCLUDED is a
#                                     game on nds-bootstrap's twlClockExcludeList, e.g. Sonic Rush)
# Wrap it in emu-lock: it runs one emulator at a time. (Variables set for one
# call go in a subshell: sh keeps assignments made before a function call.)
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
mode=${MODE:-hc}
set_json=$repo/tests/bootstrap/sets/live.json

# ---- SD card set-up steps (called back through SD_SETUP with the SD dir last) ----
case "${1:-}" in
setup-ips)  # apFix/<romname>.ips writing the marker into the ARM9 binary
  mkdir -p "$2/_nds/nds-bootstrap/apFix"
  printf 'PATCH\000\100\004\000\004DSCXEOF' > "$2/_nds/nds-bootstrap/apFix/game.nds.ips"; exit 0 ;;
setup-bin)  # apFix/<romname>.bin: the cheat-code form, one 32-bit write
  mkdir -p "$2/_nds/nds-bootstrap/apFix"
  python3 -c 'import struct,sys; open(sys.argv[1],"wb").write(struct.pack("<II",0x02000004,0x58435344))' \
    "$2/_nds/nds-bootstrap/apFix/game.nds.bin"; exit 0 ;;
setup-widecheat)
  mkdir -p "$2/_nds/nds-bootstrap"
  python3 -c 'import struct,sys; open(sys.argv[1],"wb").write(struct.pack("<II",0x02000004,0x58435344))' \
    "$2/_nds/nds-bootstrap/wideCheatData.bin"; exit 0 ;;
setup-srl)  # softResetParams.bin asks for the SRL appended to the ROM file at $SRL_OFFSET
  mkdir -p "$2/_nds/nds-bootstrap"
  python3 -c 'import struct,sys; open(sys.argv[1],"wb").write(struct.pack("<4I",0xFFFFFFFF,0,0,int(sys.argv[2])) + bytes(0x40))' \
    "$2/_nds/nds-bootstrap/softResetParams.bin" "$SRL_OFFSET"; exit 0 ;;
setup-donor)  # DONOR20_NDS_PATH set to a copy of the ROM
  cp "$ROM" "$2/games/donor.nds"
  echo "DONOR20_NDS_PATH = sd:/games/donor.nds" >> "$2/_nds/nds-bootstrap.ini"; exit 0 ;;
setup-boost)  # the player's speed-ups, set before launch
  printf 'BOOST_CPU = 1\nBOOST_VRAM = 1\n' >> "$2/_nds/nds-bootstrap.ini"; exit 0 ;;
setup-dsimode2)  # DSi mode forced on a DS game: its TWL clock
  echo 'DSI_MODE = 2' >> "$2/_nds/nds-bootstrap.ini"; exit 0 ;;
setup-cache)  # point the cached card-read patch offset at the marker
  python3 - "$2" <<'EOF'
import glob, struct, sys
f = glob.glob(sys.argv[1] + "/_nds/nds-bootstrap/patchOffsetCache/YKWE-*.bin")[0]
d = bytearray(open(f, "rb").read())
print("cache: ver", struct.unpack_from("<H", d, 0)[0], "cardReadStartOffset", hex(struct.unpack_from("<I", d, 0x3C)[0]), "-> 0x02000004")
struct.pack_into("<I", d, 0x3C, 0x02000004)
open(f, "wb").write(d)
EOF
  exit 0 ;;
esac

: "${DSI_FILES:?}" "${ROM:?}" "${BOOTSTRAP:?}" "${RUNNER:?}"
session_args=
[ "$mode" = casual ] && session_args=--softcore
hc=1; [ "$mode" = casual ] && hc=0
hash=${HASH:-6d77d9f9b34323e4060206b675620b8d}   # Kirby Super Star Ultra (USA); only the module uses it

marker() {  # marker RAM.BIN -> applied / not applied
  python3 -c 'import sys; d=open(sys.argv[1],"rb").read(8); print("applied" if d[4:8]==b"DSCX" else "not applied (%s)" % d[4:8].hex())' "$1" 2>/dev/null || echo "no RAM dump"
}
boot() {  # boot WORK FRAMES [SD_SETUP step]: nds-bootstrap with a fresh session
  WORK=$1 SESSION_ARGS="$session_args ${EXTRA_SESSION_ARGS:-}" SD_SETUP="${3:+$0 $3}" \
    "$repo/tests/bootstrap/run.sh" "$hash" "$set_json" "$2" > "$1/out.txt" 2>&1
}
used() { [ -f "$1/sd/_nds/dscheevos/session.used" ] && echo "session used" || echo "session NOT used"; }
lboot() {  # lboot WORK FRAMES [SD_SETUP step]: the launcher flow (presses A now and then for prompts)
  printf '' > "$1/keys.txt"; f=400; while [ $f -le 2000 ]; do echo "$f 0x1 4" >> "$1/keys.txt"; f=$((f + 100)); done
  WORK=$1 HARDCORE=$hc SCRIPT="$1/keys.txt" SD_SETUP="${3:+$0 $3}" \
    "$repo/tests/launcher/run.sh" "$set_json" "$2" > "$1/out.txt" 2>&1
}
server_mode() { grep -o '"event": "startsession"[^}]*' "$1/out.txt" | grep -o 'hardcore": [a-z]*' | head -1; }

for c in "$@"; do
  w=$(mktemp -d)
  case "$c" in
  ips|bin|widecheat)
    boot "$w" 1800 "setup-$c"
    echo "$c ($mode): $(marker "$w/ram.bin"); $(used "$w")" ;;
  srl)
    : "${SRL:?srl needs SRL=file.nds}"
    rom=$w/rom.nds; cp "$ROM" "$rom"
    size=$(wc -c < "$rom"); SRL_OFFSET=$(( (size + 0x1FF) / 0x200 * 0x200 )); export SRL_OFFSET
    dd if=/dev/zero bs=1 count=$((SRL_OFFSET - size)) >> "$rom" 2>/dev/null; cat "$SRL" >> "$rom"
    (ROM=$rom; boot "$w" 1800 setup-srl)
    tid=$(python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); print(d[0x7FFE0C:0x7FFE10])' "$w/ram.bin" 2>/dev/null)
    echo "srl ($mode): booted header TID $tid (the ROM's is YKWE); $(used "$w")" ;;
  cache)
    boot "$w" 1800                                   # makes the cache for this game
    rm -f "$w/sd/_nds/dscheevos/session.used" "$w/ram.bin"
    boot "$w" 1800 setup-cache
    echo "cache ($mode): $(grep '^cache:' "$w/out.txt"); card read patch at the marker: $(python3 -c 'import sys; d=open(sys.argv[1],"rb").read(8); print("yes, game code overwritten" if d[4:8]!=bytes.fromhex("ffdeffe7") else "no")' "$w/ram.bin")" ;;
  stale)  # a session written an hour ago, as a saved copy would be
    (EXTRA_SESSION_ARGS="--unix-time $(python3 -c 'import calendar,time; print(calendar.timegm(time.localtime()) - 3600)')"; boot "$w" 1800)
    echo "stale ($mode): $(used "$w")" ;;
  wipe)
    boot "$w" 1800
    echo "wipe ($mode): $(used "$w"), session.used starts $(head -c 8 "$w/sd/_nds/dscheevos/session.used" 2>/dev/null | xxd -p)" ;;
  launcher-ips)
    : "${ROM_USERONLY:?launcher-ips needs ROM_USERONLY}"
    (ROM=$ROM_USERONLY; lboot "$w" 2400 setup-ips)
    echo "launcher-ips (asked $mode): $(marker "$w/ram.bin"); server session $(server_mode "$w"); launcher: $(grep -h 'apfix' "$w/sd/_nds/dscheevos/launcher.log" 2>/dev/null | head -1)" ;;
  launcher-builtin)
    lboot "$w" 2400 setup-ips
    echo "launcher-builtin (asked $mode): $(marker "$w/ram.bin"); server session $(server_mode "$w"); launcher: $(grep -h 'apfix' "$w/sd/_nds/dscheevos/launcher.log" 2>/dev/null | head -1)" ;;
  launcher-donor)  # the ROM's header made to look like the Yoshi's Island DS build that uses a donor
    rom=$w/ayi.nds; cp "$ROM" "$rom"
    python3 -c 'import struct,sys; f=open(sys.argv[1],"r+b"); f.seek(0xC); f.write(b"AYIE"); f.seek(0x3C); f.write(struct.pack("<I",0x25F70))' "$rom"
    (ROM=$rom; lboot "$w" 1800 setup-donor)
    echo "launcher-donor (asked $mode): server session $(server_mode "$w"); launcher: $(grep -h 'donor' "$w/sd/_nds/dscheevos/launcher.log" 2>/dev/null | head -1)" ;;
  boost|dsimode2|boost-excluded)
    step=setup-$c; [ "$c" = boost-excluded ] && step=setup-boost
    if [ "$c" = boost-excluded ]; then (ROM=${ROM_EXCLUDED:?}; boot "$w" 1800 $step); else boot "$w" 1800 $step; fi
    log=$(tr -cd '[:print:]\n' < "$w/sd/NDSBTSRP.LOG" 2>/dev/null)
    echo "$c ($mode): front end $(echo "$log" | grep -m1 '^boostCpu' | tr -d '"') $(echo "$log" | grep -m1 '^dsiMode' | tr -d '"'); module $(grep -m1 -E '^speed-ups|module not loaded' "$w/out.txt" || echo 'no status'); $(grep -m1 -o 'speed-ups 0x[0-9a-f]*' "$w/out.txt" | sed 's/^/journal /')" ;;
  *) echo "unknown case $c" ;;
  esac
  [ -n "${KEEP:-}" ] && echo "  kept $w" || rm -rf "$w"
done
