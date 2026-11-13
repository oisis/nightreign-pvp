#!/bin/sh
# Assemble the distributable folders and archives in $W/dist/packages: launcher + data + me3 + README.
# Run launcher/build.sh first. me3 comes from $W/tools/me3 (Linux) and $W/tools/dl/me3-windows-amd64.zip.
# Names carry the version from the repository's VERSION file: NightreignInvasions-<version>-<os>.
set -e
. "$(dirname "$0")/../re/common.sh"
V=$(tr -d '[:space:]' < "$R/VERSION")
D="$W/dist"
OUT="$D/packages"
LIN="NightreignInvasions-$V-linux"
WIN="NightreignInvasions-$V-windows"
rm -rf "$OUT"
mkdir -p "$OUT/$LIN/me3" "$OUT/$WIN/me3"

cp "$D/linux/nightreign-invasions" "$OUT/$LIN/"
cp -r "$D/linux/data" "$OUT/$LIN/"
cp -r "$W/tools/me3/bin" "$W/tools/me3/LICENSE-APACHE" "$W/tools/me3/LICENSE-MIT" "$OUT/$LIN/me3/"

cp "$D/windows/NightreignInvasions.exe" "$OUT/$WIN/"
cp -r "$D/windows/data" "$OUT/$WIN/"
bsdtar -xf "$W/tools/dl/me3-windows-amd64.zip" -C "$OUT/$WIN/me3" bin LICENSE-APACHE LICENSE-MIT

for d in "$LIN" "$WIN"; do cp "$R/launcher/README.txt" "$R/VERSION" "$OUT/$d/"; done
(cd "$OUT" && tar czf "$LIN.tar.gz" "$LIN" && bsdtar -a -cf "$WIN.zip" "$WIN")
ls -la "$OUT"/*.tar.gz "$OUT"/*.zip
