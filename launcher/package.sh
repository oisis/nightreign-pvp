#!/bin/sh
# Assemble the distributable folders and archives in $W/dist/packages: launcher + data + me3 + README.
# Run launcher/build.sh first. me3 comes from $W/tools/me3 (Linux) and $W/tools/dl/me3-windows-amd64.zip.
set -e
. "$(dirname "$0")/../re/common.sh"
D="$W/dist"
OUT="$D/packages"
rm -rf "$OUT"
mkdir -p "$OUT/NightreignInvasions-linux/me3" "$OUT/NightreignInvasions-windows/me3"

cp "$D/linux/nightreign-invasions" "$OUT/NightreignInvasions-linux/"
cp -r "$D/linux/data" "$OUT/NightreignInvasions-linux/"
cp -r "$W/tools/me3/bin" "$W/tools/me3/LICENSE-APACHE" "$W/tools/me3/LICENSE-MIT" "$OUT/NightreignInvasions-linux/me3/"

cp "$D/windows/NightreignInvasions.exe" "$OUT/NightreignInvasions-windows/"
cp -r "$D/windows/data" "$OUT/NightreignInvasions-windows/"
bsdtar -xf "$W/tools/dl/me3-windows-amd64.zip" -C "$OUT/NightreignInvasions-windows/me3" bin LICENSE-APACHE LICENSE-MIT

for d in linux windows; do cp "$R/launcher/README.txt" "$OUT/NightreignInvasions-$d/"; done
(cd "$OUT" && tar czf NightreignInvasions-linux.tar.gz NightreignInvasions-linux && bsdtar -a -cf NightreignInvasions-windows.zip NightreignInvasions-windows)
ls -la "$OUT"/*.tar.gz "$OUT"/*.zip
