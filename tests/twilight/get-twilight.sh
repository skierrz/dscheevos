#!/bin/sh
# Download TWiLight Menu++ (DSi package, pinned release, SHA-256 checked), keep
# it in a cache, and extract it into DIR. It is never kept in the repo.
#   TWL_VERSION, TWL_SHA256  another release (TWiLightMenu-DSi.7z) and its hash
#   TWL_CACHE  where the download is kept (default ~/.cache/dscheevos)
# Usage: get-twilight.sh DIR
set -eu
dir=$1
version=${TWL_VERSION:-v27.24.1}
sha256=${TWL_SHA256:-c04fc66305ce8dc80e69aa4070d3ce966f04868881663f57b91f68d687a2bb90}
cache=${TWL_CACHE:-$HOME/.cache/dscheevos}
mkdir -p "$cache"
archive=$cache/TWiLightMenu-DSi-$version.7z
if [ ! -f "$archive" ]; then
  curl -fsSL -o "$archive.part" "https://github.com/DS-Homebrew/TWiLightMenu/releases/download/$version/TWiLightMenu-DSi.7z"
  mv "$archive.part" "$archive"
fi
# macOS's sha256sum has no --quiet and older macOS has none at all: compare by hand
actual=$( (sha256sum "$archive" 2>/dev/null || shasum -a 256 "$archive") | cut -d' ' -f1)
[ "$actual" = "$sha256" ] || { echo "$archive: SHA-256 is $actual, expected $sha256" >&2; exit 1; }
rm -rf "$dir" && mkdir -p "$dir"
# p7zip installs 7z; Homebrew's sevenzip only installs 7zz
sevenz=$(command -v 7z || command -v 7zz) || { echo "need 7z or 7zz" >&2; exit 1; }
"$sevenz" x -y -o"$dir" "$archive" > /dev/null
