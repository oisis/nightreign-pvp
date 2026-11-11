#!/bin/sh
# Build the launcher for Linux (GTK 3) and Windows (Win32) into $W/dist/linux and $W/dist/windows.
# Needs gcc + GTK 3 headers, zig in $W/tools, and mod/build.sh run first.
set -e
. "$(dirname "$0")/../re/common.sh"
ZIG="$W/tools/zig/zig"
D="$W/dist"
L="$R/launcher"
mkdir -p "$D/linux/data" "$D/windows/data"

gcc -O2 -Wall -Wextra -o "$D/linux/nightreign-invasions" "$L/gtk.c" "$L/core.c" $(pkg-config --cflags --libs gtk+-3.0)
gcc -O2 -Wall -Wextra -o "$D/linux/cli" "$L/cli.c" "$L/core.c"
"$ZIG" cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Wl,--subsystem,windows -o "$D/windows/NightreignInvasions.exe" \
  "$L/win.c" "$L/core.c" -lole32 -lshell32 -ladvapi32 -lgdi32
"$ZIG" cc -target x86_64-windows-gnu -O2 -o "$D/windows/cli.exe" "$L/cli.c" "$L/core.c" -ladvapi32
find "$D" -maxdepth 3 \( -name '*.pdb' -o -name '*.lib' \) -delete

for d in linux windows; do cp "$W/build/t600001000.delta" "$W/build/native/invasion.dll" "$D/$d/data/"; done
cp "$W/build/unkrak.exe" "$D/linux/data/"
echo "launcher built in $D/linux and $D/windows"
