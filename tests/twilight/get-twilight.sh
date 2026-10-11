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
echo "$sha256  $archive" | sha256sum -c --quiet
rm -rf "$dir" && mkdir -p "$dir"
7z x -y -o"$dir" "$archive" > /dev/null
