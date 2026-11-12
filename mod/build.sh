#!/bin/sh
# Developer build of the mod into $W/build (see re/common.sh for R and W):
#   native/invasion.dll        the client DLL
#   regulation.bin             the game's params plus our Target list row
#   msg/<lang>/menu_dlc01…     menu texts, for the languages extracted to analysis/gamefiles
#   menu/21_common_dlc01_…     the DLC menu icon atlas plus our Target list icon (needs Pillow)
#   unkrak.exe                 Oodle helper the Linux launcher ships
# and $W/build-launcher-data/icon_<id>.bc7, our icons as encoded texture blocks, which the launcher
# ships (it builds everything else from the player's own game files). That folder is kept outside
# build/ because build/ is what me3 overlays on the game.
# NR_GAME: the game's "Game" folder if it is not in the default Steam library.
set -e
. "$(dirname "$0")/../re/common.sh"
ZIG="$W/tools/zig/zig"
OODLE="$W/tools/esdtool/esdtool-v0.5.1/oo2core_9_win64.dll"   # the game's own DLL, copied locally
GAME="${NR_GAME:-$HOME/.local/share/Steam/steamapps/common/ELDEN RING NIGHTREIGN/Game}"
B="$W/build"
rm -rf "$B/script" "$B/talk-src" "$B/t600001000.delta"
mkdir -p "$B/native" "$B/tmp"

DLL="-target x86_64-windows-gnu -shared -O2 -Wall -Wextra"
"$ZIG" cc $DLL -o "$B/native/invasion.dll" "$R/mod/dll/invasion.c"
"$ZIG" cc -target x86_64-windows-gnu -O2 -o "$B/unkrak.exe" "$R/mod/tools/unkrak.c"

# Test sessions without the rain's damage: an empty file "no-rain" next to the DLL (build/native/).
NO_RAIN=""; [ -e "$B/native/no-rain" ] && NO_RAIN="--no-rain"
uv run -q "$R/mod/build_regulation.py" "$GAME/regulation.bin" "$B/regulation.bin" $NO_RAIN

# Menu texts for the development package (the launcher does this itself for all languages).
for src in "$W"/analysis/gamefiles/msg/*/menu_dlc01.msgbnd.dcx; do
  lang=$(basename "$(dirname "$src")")
  (cd "$B" && wine_run unkrak.exe "$(w "$OODLE")" "$(w "$src")" "$(w "$B/tmp/menu-$lang.bin")")
  python3 "$R/mod/build_msg.py" "$lang" "$B/tmp/menu-$lang.bin" "$B/msg/$lang/menu_dlc01.msgbnd.dcx"
done

# Our Target list icon, drawn into the DLC icon atlas (high and low resolution sets).
unkrak() { (cd "$B" && wine_run unkrak.exe "$(w "$OODLE")" "$(w "$1")" "$(w "$2")"); }
set --
for q in h l; do
  for kind in sblytbnd tpf; do
    unkrak "$W/analysis/gamefiles/menu/21_common_dlc01_$q.$kind.dcx" "$B/tmp/icons_$q.$kind"
  done
  set -- "$@" "$B/tmp/icons_$q.sblytbnd" "$B/tmp/icons_$q.tpf" \
    "$B/menu/21_common_dlc01_$q.sblytbnd.dcx" "$B/menu/21_common_dlc01_$q.tpf.dcx"
done
python3 "$R/mod/build_icon.py" "$@"
python3 "$R/mod/build_icon.py" --blocks "$W/build-launcher-data"
rm -rf "$B/tmp"
find "$B" -maxdepth 2 \( -name '*.pdb' -o -name '*.lib' \) -delete
echo "mod built in $B"
