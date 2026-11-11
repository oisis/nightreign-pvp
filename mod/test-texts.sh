#!/bin/sh
# Development: isolate the start-up crash. Profile: $W/invasion-texts.me3 (see analysis notes).
. "$(dirname "$0")/../re/common.sh"
exec "$W/tools/me3/bin/me3" --windows-binaries-dir "$W/tools/me3/bin/win64" launch -g nightreign -p "$W/invasion-texts.me3" "$@"
