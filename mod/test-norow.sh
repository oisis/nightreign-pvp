#!/bin/sh
# Development: like test.sh, but with a regulation.bin that is only repacked (no added row).
# Tells a packing problem from a param problem. Profile: $W/invasion-norow.me3.
. "$(dirname "$0")/../re/common.sh"
exec "$W/tools/me3/bin/me3" --windows-binaries-dir "$W/tools/me3/bin/win64" launch -g nightreign -p "$W/invasion-norow.me3" "$@"
