#!/bin/sh
# Developer check before a package is handed over: the launcher's CLI built with AddressSanitizer and
# UndefinedBehaviorSanitizer runs its selftest and a FULL build of the mod files from the real game files,
# into a scratch copy of an install (never the player's own folder). Any sanitizer report fails the check.
#   launcher/asan-check.sh <scratch app dir> [game dir]
set -e
. "$(dirname "$0")/../re/common.sh"
APP="$1"; GAME="$2"
[ -n "$APP" ] || { echo "usage: $0 <scratch app dir> [game dir]"; exit 2; }
O="$W/dist/obj"
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
    -o "$O/cli-asan" "$R/launcher/cli.c" "$R/launcher/core.c" "$O/zstd-linux.o"
mkdir -p "$APP/work"
rm -f "$APP/work/build.stamp"
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
"$O/cli-asan" "$APP" selftest | tail -1
if [ -n "$GAME" ]; then "$O/cli-asan" "$APP" build "$GAME"; else "$O/cli-asan" "$APP" build; fi | tail -3
echo "asan check: selftest and full build passed"
