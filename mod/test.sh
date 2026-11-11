#!/bin/sh
# Development: launch Nightreign with the mod from $W/build through me3 (offline, separate save).
# Run from a desktop session; Steam must be running. Profile: $W/invasion.me3.
. "$(dirname "$0")/../re/common.sh"
exec "$W/tools/me3/bin/me3" --windows-binaries-dir "$W/tools/me3/bin/win64" launch -g nightreign -p "$W/invasion.me3" "$@"
