#!/bin/sh
# Developer build of the mod into $W/build (see re/common.sh for R and W):
#   native/invasion.dll        the client DLL (with the Target list hook)
#   regulation.bin             the game's params plus our Target list row
#   msg/<lang>/menu_dlc01…     menu texts, for the languages extracted to analysis/gamefiles
#   unkrak.exe                 Oodle helper the Linux launcher ships
# Also builds $W/build/package-dll/invasion.dll: the DLL without the Target list hook, which is what
# the launcher packages until the launcher can produce regulation.bin itself.
# NR_GAME: the game's "Game" folder if it is not in the default Steam library.
set -e
. "$(dirname "$0")/../re/common.sh"
ZIG="$W/tools/zig/zig"
OODLE="$W/tools/esdtool/esdtool-v0.5.1/oo2core_9_win64.dll"   # the game's own DLL, copied locally
GAME="${NR_GAME:-$HOME/.local/share/Steam/steamapps/common/ELDEN RING NIGHTREIGN/Game}"
B="$W/build"
rm -rf "$B/script" "$B/talk-src" "$B/t600001000.delta"
mkdir -p "$B/native" "$B/package-dll" "$B/tmp"

DLL="-target x86_64-windows-gnu -shared -O2 -Wall -Wextra"
"$ZIG" cc $DLL -DTARGET_MENU -o "$B/native/invasion.dll" "$R/mod/dll/invasion.c"
"$ZIG" cc $DLL -o "$B/package-dll/invasion.dll" "$R/mod/dll/invasion.c"
"$ZIG" cc -target x86_64-windows-gnu -O2 -o "$B/unkrak.exe" "$R/mod/tools/unkrak.c"

uv run -q "$R/mod/build_regulation.py" "$GAME/regulation.bin" "$B/regulation.bin"

# Menu texts for the development package (the launcher does this itself for all languages).
for src in "$W"/analysis/gamefiles/msg/*/menu_dlc01.msgbnd.dcx; do
  lang=$(basename "$(dirname "$src")")
  (cd "$B" && wine_run unkrak.exe "$(w "$OODLE")" "$(w "$src")" "$(w "$B/tmp/menu-$lang.bin")")
  python3 "$R/mod/build_msg.py" "$lang" "$B/tmp/menu-$lang.bin" "$B/msg/$lang/menu_dlc01.msgbnd.dcx"
done
rm -rf "$B/tmp"
find "$B" -maxdepth 2 \( -name '*.pdb' -o -name '*.lib' \) -delete
echo "mod built in $B"
