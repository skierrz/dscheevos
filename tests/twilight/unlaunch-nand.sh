#!/bin/sh
# Make an emulator NAND copy with Unlaunch installed and set to start
# sdmc:/BOOT.NDS (TWiLight Menu++) when no button is held, for run.sh BOOT=unlaunch.
# Only the copy (OUT) is ever written; never use this on a console's NAND.
#
# Unlaunch is freeware by Martin Korth (problemkaputt.de), downloaded here at a
# pinned version and checked against a SHA-256; it is never kept in the repo.
# It is installed the way its page describes for a hardmod or no$gba: UNLAUNCH.DSI
# appended to the launcher's title.tmd. Then the copy is cold-booted once and,
# in Unlaunch's OPTIONS, NO BUTTON is set to sdmc:/BOOT.NDS and saved, as a user
# would. During that boot the SD holds only BOOT.NDS, so it is the last entry in
# Unlaunch's list whatever DSiWare the NAND has.
# Needs:
#   DSI_FILES      dir with biosnds7/9.bin, biosdsi7/9.bin, dsifirmware.bin, dsinand.bin (your own dumps)
#   RUNNER         melonds-headless from skierrz/melonDS (dscheevos branch)
#   MELONDS_SRC    that melonDS source tree (default: tools/melonDS)
#   MELONDS_BUILD  the CMake build dir of melonds-headless (for its libcore.a), to build nandtool
# Optional: WORK, UNLAUNCH_CACHE (default ~/.cache/dscheevos), TWL_* (see get-twilight.sh)
# Usage: unlaunch-nand.sh OUT.bin
set -eu
: "${DSI_FILES:?}" "${RUNNER:?}" "${MELONDS_BUILD:?}"
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
out=$1
src=${MELONDS_SRC:-$repo/tools/melonDS}
. "$here/../lib/workdir.sh"
work=${WORK:-$(dsc_mkwork dsc-un-$$)}
[ -n "${WORK:-}" ] || trap 'dsc_rmwork "$work"' EXIT   # no WORK: delete on exit
cache=${UNLAUNCH_CACHE:-$HOME/.cache/dscheevos}
mkdir -p "$work" "$cache"
bios7i=$DSI_FILES/biosdsi7.bin

# --- Unlaunch v2.0
zip=$cache/unlaunch-2.0.zip
if [ ! -f "$zip" ]; then
  curl -fsSL -o "$zip.part" https://problemkaputt.de/unlaunch.zip
  mv "$zip.part" "$zip"
fi
want=d0dbe7705af1aa02f5ab01e9583568b006d5d8ec9ccf291e7248ebb4f520b2cf
actual=$( (sha256sum "$zip" 2>/dev/null || shasum -a 256 "$zip") | cut -d' ' -f1)  # macOS sha256sum has no --quiet
[ "$actual" = "$want" ] || { echo "$zip: SHA-256 is $actual, expected $want" >&2; exit 1; }
python3 -I -c 'import sys, zipfile; open(sys.argv[2], "wb").write(zipfile.ZipFile(sys.argv[1]).read("UNLAUNCH.DSI"))' "$zip" "$work/UNLAUNCH.DSI"

# --- nandtool, linked against the runner's melonDS core
g++ -std=c++17 -O1 -DARCHITECTURE_x86_64=1 -I"$src/src" -I"$MELONDS_BUILD/melonds/src" -I"$src/src/teakra/include" \
  "$here/nandtool.cpp" "$src/tools/dscheevos/headless/platform.cpp" -o "$work/nandtool" \
  "$MELONDS_BUILD/melonds/src/libcore.a" "$MELONDS_BUILD/melonds/src/teakra/src/libteakra.a" \
  "$MELONDS_BUILD/net/libnet-utils.a" "$MELONDS_BUILD/net/libslirp/libslirp.a" $(pkg-config --libs glib-2.0) -lpthread
nt() { "$work/nandtool" "$bios7i" "$out" "$@"; }

# --- install: append UNLAUNCH.DSI to the launcher's title.tmd (in the copy)
cp "$DSI_FILES/dsinand.bin" "$out"
launcher=$(nt titles 00030017)
[ "$(echo "$launcher" | wc -w)" = 1 ] || { echo "expected one launcher title, got: $launcher" >&2; exit 1; }
tmd=0:/title/00030017/$launcher/content/title.tmd
nt export "$tmd" "$work/title.tmd" > /dev/null
[ "$(wc -c < "$work/title.tmd")" = 520 ] || { echo "title.tmd isn't the stock 520 bytes; already modified?" >&2; exit 1; }
cat "$work/title.tmd" "$work/UNLAUNCH.DSI" > "$work/title-unlaunch.tmd"
nt import "$tmd" "$work/title-unlaunch.tmd" > /dev/null

# --- first boot: OPTIONS -> NO BUTTON -> sdmc:/BOOT.NDS -> SAVE & EXIT
"$here/get-twilight.sh" "$work/twl"
rm -rf "$work/sd" "$work/sd.img" && mkdir -p "$work/sd"
cp "$work/twl/BOOT.NDS" "$work/sd/"
python3 -I -c '
import sys
DOWN, A = 0x80, 0x1
k = [(300, DOWN, 6), (330, DOWN, 6), (360, A, 6)]   # filemenu: cart, WIFIBOOT, OPTIONS
k += [(420, A, 6)]                                  # NO BUTTON: pick a title
k += [(480, DOWN, 900), (1400, A, 6)]               # last entry: sdmc:/BOOT.NDS
k += [(1440 + i * 30, DOWN, 6) for i in range(7)]   # down to SAVE & EXIT (the list stops there)
k += [(1700, A, 6)]
open(sys.argv[1], "w").writelines("%d %#x %d\n" % x for x in k)' "$work/unlaunch-keys.txt"
"$RUNNER" $(dsc_runner_flags) --mode dsi --rom "$work/sd/BOOT.NDS" --firmware-boot --bios-dir "$DSI_FILES" --nand "$out" \
  --sd "$work/sd" --frames 1900 --script "$work/unlaunch-keys.txt" > "$work/unlaunch-setup.log" 2>&1

# --- check the saved NO BUTTON entry
nt export "$tmd" "$work/title-saved.tmd" > /dev/null
python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read()
want = "sdmc:/BOOT.NDS".encode("utf-16-le")
if d[0x208:0x218] != b"UNLAUNCH OPTIONS" or d[0x278:0x278 + len(want)] != want:
    sys.exit("RESULT FAIL: NO BUTTON is not sdmc:/BOOT.NDS")
print("RESULT PASS: Unlaunch installed, NO BUTTON = sdmc:/BOOT.NDS")' "$work/title-saved.tmd"
