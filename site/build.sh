#!/bin/sh
# Build the site image: the info page plus the launcher packages from $W/dist/packages.
#   site/build.sh [image name, default nightreign-invasions-site:0.1.0]
set -e
. "$(dirname "$0")/../re/common.sh"
S="$W/dist/site"
rm -rf "$S"
mkdir -p "$S/downloads"
cp "$R/site/index.html" "$S/"
cp "$W/dist/packages/NightreignInvasions-windows.zip" "$W/dist/packages/NightreignInvasions-linux.tar.gz" "$S/downloads/"
docker build -f "$R/site/Dockerfile" -t "${1:-nightreign-invasions-site:0.1.0}" "$S"
