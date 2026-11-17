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
# Plain sh has no pipefail: each run writes its whole output to a log, and the exit status is the CLI's own.
LOG="$APP/asan-check.log"
: > "$LOG"
fail() { tail -5 "$LOG"; echo "asan check: FAILED ($1), see $LOG"; exit 1; }
set +e
"$O/cli-asan" "$APP" selftest > "$APP/asan-selftest.log" 2>&1; rc=$?
cat "$APP/asan-selftest.log" >> "$LOG"
tail -1 "$APP/asan-selftest.log"
{ [ $rc -eq 0 ] && grep -q "selftest ok" "$APP/asan-selftest.log"; } || fail selftest
if [ -n "$GAME" ]; then "$O/cli-asan" "$APP" build "$GAME"; else "$O/cli-asan" "$APP" build; fi > "$APP/asan-build.log" 2>&1; rc=$?
cat "$APP/asan-build.log" >> "$LOG"
tail -3 "$APP/asan-build.log"
{ [ $rc -eq 0 ] && grep -q "Mod files are ready" "$APP/asan-build.log"; } || fail build
echo "asan check: selftest and full build passed"
