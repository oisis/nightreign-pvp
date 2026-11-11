#!/bin/sh
# Run ESDLang's esdtool.exe (talk script decompiler/compiler) under wine with the work folder's
# prefix and the unpacked Windows .NET 6 runtime. Paths passed to it must be Windows-style (Z:\home\...).
# Decompile:  esdtool.sh -nr -noannotate -i 'Z:\...\m10_00_00_00.talkesdbnd.dcx' -writepy 'Z:\...\out\%e.py'
. "$(dirname "$0")/common.sh"
cd "$W/tools/esdtool/esdtool-v0.5.1" || exit 1
WINEPREFIX="$W/tools/wineprefix" WINEDEBUG=-all DOTNET_ROOT="$(w "$W/tools/dotnet6-win")" exec wine esdtool.exe "$@"
