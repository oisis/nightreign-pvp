#!/bin/sh
# Developer build of the mod into $W/build (see re/common.sh for R and W):
#   native/invasion.dll            the client DLL
#   script/talk/…talkesdbnd.dcx    the hub's talk bundle with our table script
#   msg/<lang>/menu_dlc01…         menu texts, for the languages extracted to analysis/gamefiles
#   regulation.bin                 experiment: "Invasion" entry in the Target list
#   t600001000.delta, unkrak.exe   what the launcher ships
# Needs the game's own files extracted first (re/bhd_extract.py), because the table script is kept
# in this repository only as a patch against the game's script:
#   mod/build.sh [vanilla m10_00_00_00.talkesdbnd.dcx]
# DLL_DEFS: extra compiler flags, e.g. DLL_DEFS=-DTRY_CREATE for the create-session experiment.
set -e
. "$(dirname "$0")/../re/common.sh"
VANILLA=$(realpath "${1:-$W/analysis/gamefiles/script/talk/m10_00_00_00.talkesdbnd.dcx}")
ZIG="$W/tools/zig/zig"
OODLE="$W/tools/esdtool/esdtool-v0.5.1/oo2core_9_win64.dll"   # the game's own DLL, copied locally
B="$W/build"
mkdir -p "$B/native" "$B/talk-src"

"$ZIG" cc -target x86_64-windows-gnu -shared -O2 -Wall -Wextra ${DLL_DEFS:-} -o "$B/native/invasion.dll" "$R/mod/dll/invasion.c"
"$ZIG" cc -target x86_64-windows-gnu -O2 -o "$B/unkrak.exe" "$R/mod/tools/unkrak.c"

# Table script: decompile the game's, apply our patch, compile back into a copy of the bundle.
"$R/re/esdtool.sh" -nr -noannotate -i "$(w "$VANILLA")" -f t600001000 -writepy "$(w "$B/talk-src")\\%e.orig.py" >/dev/null
tr -d '\r' < "$B/talk-src/t600001000.orig.py" > "$B/talk-src/t600001000.py"
patch -s "$B/talk-src/t600001000.py" "$R/mod/talk/t600001000.patch"
"$R/re/esdtool.sh" -nr -noannotate -i "$(w "$VANILLA")" -i "$(w "$B/talk-src/t600001000.py")" -writebnd "$(w "$B/script/talk")" >/dev/null

# Delta of the compiled script against the game's, for the launcher.
unkrak() { (cd "$B" && wine_run unkrak.exe "$(w "$OODLE")" "$(w "$1")" "$(w "$2")"); }
unkrak "$VANILLA" "$B/talk-src/vanilla.bin"
unkrak "$B/script/talk/m10_00_00_00.talkesdbnd.dcx" "$B/talk-src/ours.bin"
python3 "$R/mod/make_delta.py" "$B/talk-src/vanilla.bin" "$B/talk-src/ours.bin" '\t600001000.esd' "$B/t600001000.delta"

# Menu texts for the development package (the launcher does this itself for all languages).
for src in "$W"/analysis/gamefiles/msg/*/menu_dlc01.msgbnd.dcx; do
  lang=$(basename "$(dirname "$src")")
  unkrak "$src" "$B/talk-src/menu-$lang.bin"
  python3 "$R/mod/build_msg.py" "$lang" "$B/talk-src/menu-$lang.bin" "$B/msg/$lang/menu_dlc01.msgbnd.dcx"
done
# Experiment: "Invasion" entry in the expedition menu's Target list (development package only).
uv run -q "$R/mod/build_regulation.py" "${NR_GAME:-$HOME/.local/share/Steam/steamapps/common/ELDEN RING NIGHTREIGN/Game}/regulation.bin" "$B/regulation.bin"

find "$B" -maxdepth 2 \( -name '*.pdb' -o -name '*.lib' \) -delete
echo "mod built in $B"
