#!/bin/sh
# Assemble the SD card folder for a DScheevos release from built parts.
#   package.sh OUT_DIR VERSION
# Expects, relative to the repo root:
#   dscheevos/launcher/dscheevos-launcher.nds
#   dscheevos/launcher/loader/out/load.bin
#   dscheevos/module/build/dscheevos9.bin
#   external/nds-bootstrap/bin/nds-bootstrap-nightly.nds
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
out=$1 version=$2
sd=$out/sd
rm -rf "$out"
mkdir -p "$sd/_nds/dscheevos/licenses"

cp "$repo/dscheevos/launcher/dscheevos-launcher.nds" "$sd/_nds/nds-bootstrap-release.nds"
cp "$repo/dscheevos/launcher/dscheevos-launcher.nds" "$sd/_nds/nds-bootstrap-nightly.nds"
cp "$repo/external/nds-bootstrap/bin/nds-bootstrap-nightly.nds" "$sd/_nds/dscheevos/nds-bootstrap.nds"
cp "$repo/dscheevos/module/build/dscheevos9.bin" "$repo/dscheevos/launcher/loader/out/load.bin" "$sd/_nds/dscheevos/"
printf '%s\n' "$version" > "$sd/_nds/dscheevos/version.txt"

# Licenses of what the binaries contain
lic=$sd/_nds/dscheevos/licenses
cp "$repo/external/rcheevos/LICENSE" "$lic/rcheevos.txt"
cp "$repo/external/mbedtls/LICENSE" "$lic/mbedtls.txt"
# nds-bootloader states GPL v2 or later in its source headers and ships no license file
{ echo "devkitPro nds-bootloader: GNU General Public License, version 2 or (at your option) any later version."; echo; cat /usr/share/common-licenses/GPL-2; } > "$lic/nds-bootloader.txt"
cp "$repo/external/nds-bootstrap/LICENSE" "$lic/nds-bootstrap.txt"
cat > "$lic/SOURCES.txt" <<SRC
DScheevos $version is built from:
  https://github.com/skierrz/dscheevos ($version)
  https://github.com/skierrz/nds-bootstrap (dscheevos branch, $(git -C "$repo/external/nds-bootstrap" rev-parse --short HEAD))
  https://github.com/devkitPro/nds-bootloader ($(git -C "$repo/external/nds-bootloader" rev-parse --short HEAD))
  https://github.com/RetroAchievements/rcheevos ($(git -C "$repo/external/rcheevos" rev-parse --short HEAD))
  https://github.com/Mbed-TLS/mbedtls ($(git -C "$repo/external/mbedtls" describe --tags 2>/dev/null || git -C "$repo/external/mbedtls" rev-parse --short HEAD))
The launcher is built with BlocksDS (libnds, DSWiFi with lwIP), the module and
nds-bootstrap with devkitARM; their licenses ship with those toolkits.
SRC

cp "$repo/release/README.txt" "$out/README.txt"
(cd "$out" && zip -qr "../dscheevos-$version.zip" README.txt sd)
echo "packed $(cd "$out/.." && pwd)/dscheevos-$version.zip"
