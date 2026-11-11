#!/bin/sh
# Development: like test.sh, plus Seamless Co-op (profile $W/invasion-seamless.me3).
. "$(dirname "$0")/../re/common.sh"
exec "$W/tools/me3/bin/me3" --windows-binaries-dir "$W/tools/me3/bin/win64" launch -g nightreign -p "$W/invasion-seamless.me3" "$@"
